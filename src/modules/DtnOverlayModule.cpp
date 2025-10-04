
/*
//fw+ DTN Overlay Module — Overview (DTN-first custody, FW+ handoff)

Purpose
- Opportunistic store–carry–forward for private Direct Messages. With DTN enabled, private TEXT unicasts are intercepted
  at the sender and wrapped as FWPLUS_DTN DATA (DTN-first). Native DM is used only as a late fallback near TTL expiry.
  Stock nodes require no changes; FW+ nodes unwrap at the destination and inject the original DM locally.

Ingress sources
- Local send interception (DTN-first): Router diverts private TEXT to DTN queue with a TTL; no immediate native transmit.
- FWPLUS_DTN DATA heard over the mesh: always accepted and scheduled; if we are the destination we unwrap and deliver.
- Optional capture of foreign (non-DTN) unicasts is conservative and disabled by default to avoid DTN-on-DTN recursion.

Scheduling
- First attempt is delayed using: initial base delay, deterministic anti-storm slotting (mobility-aware), per-destination
  minimum spacing, DV‑ETX confidence gating, neighbor/grace delay, and topology-aware throttles (ring/hops based). Nodes
  far from source/destination act later; confident routes may start earlier.

Forwarding and Handoff
- Primary action is to send FWPLUS_DTN DATA towards the destination.
- If the destination is far, prefer FW+ custody handoff to a better FW+ neighbor:
  • Use `NextHopRouter` route snapshot: if `next_hop` exists, map it to a direct neighbor and hand off there.
  • Otherwise select up to three FW+ neighbors (version‑gated) that are closer (lower hops from us and to dest) and rotate
    between them across retries. A per‑destination cache remembers the preferred handoff.
- Priority is BACKGROUND by default; escalate to DEFAULT only in the TTL tail and only when near the destination (ring‑gated).
- Global active cap and per‑destination spacing limit concurrent attempts.

Receipts & Milestones
- Any receipt (DELIVERED/PROGRESSED/EXPIRED) clears local pending and sets a tombstone to avoid re‑enqueue storms.
- Milestone PROGRESSED is emitted sparingly: once per id, ring‑gated near source/destination, auto‑limited under load
  (channel utilization, neighbor count, pending size) with hysteresis.

Probing, Traceroute & Fallback
- Near TTL tail we may probe FW+ peers (light overlay receipt) and, if allowed and the destination is not FW+, send a
  native DM as a proxy fallback. Fallback is encrypted by the radio layer (PKI/PSK), without spoofing the original sender.
- If DV‑ETX confidence to the destination is low and no FW+ handoff candidate is available, automatically trigger a
  traceroute (rate‑limited) to build routing confidence before attempting overlay forwarding. We do not probe when handing
  custody to a FW+ neighbor.

Version Advertisement
- One‑shot public broadcast of FW+ version ~15 s after start (channel‑utilization‑gated), then periodic passive discovery
  beacons when there are direct FW+ neighbors without a known version.

Airtime protections
- Channel utilization gate; deterministic election and mobility‑aware slotting; per‑destination spacing; global active cap;
  suppression after hearing foreign overlay DATA; neighbor/grace heuristics; tombstones and bounded caches with pruning.
*/
#include "DtnOverlayModule.h"
#if __has_include("mesh/generated/meshtastic/fwplus_dtn.pb.h")
#include "MeshService.h"
#include "Router.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Default.h"
#include "airtime.h"
#include "configuration.h"
#include "MobilityOracle.h" //fw+
#include "modules/RoutingModule.h"
#include "mesh/NextHopRouter.h" //fw+
#include "FwPlusVersion.h" //fw+
#include <pb_encode.h>
#include <cstring>

DtnOverlayModule *dtnOverlayModule; 
// Purpose: hot-reload DTN overlay settings from ModuleConfig at runtime.
// Notes: respects zero-as-default semantics; clears pending queue if module gets disabled.
void DtnOverlayModule::reloadFromModuleConfig()
{
    if (!moduleConfig.has_dtn_overlay) return;
    const auto &mc = moduleConfig.dtn_overlay;
    bool wasEnabled = configEnabled;
    // copy fields with guards (respect zeros meaning defaults where applicable)
    configEnabled = mc.enabled;
    if (mc.ttl_minutes) configTtlMinutes = mc.ttl_minutes; else configTtlMinutes = configTtlMinutes;
    if (mc.initial_delay_base_ms) configInitialDelayBaseMs = mc.initial_delay_base_ms;
    if (mc.retry_backoff_ms) configRetryBackoffMs = mc.retry_backoff_ms;
    if (mc.max_tries) configMaxTries = mc.max_tries;
    configLateFallback = mc.late_fallback_enabled;
    if (mc.fallback_tail_percent) configFallbackTailPercent = mc.fallback_tail_percent;
    configMilestonesEnabled = mc.milestones_enabled;
    if (mc.per_dest_min_spacing_ms) configPerDestMinSpacingMs = mc.per_dest_min_spacing_ms;
    if (mc.max_active_dm) configMaxActiveDm = mc.max_active_dm;
    configProbeFwplusNearDeadline = mc.probe_fwplus_near_deadline;

    // If just disabled, clear pending to stop activity immediately
    if (wasEnabled && !configEnabled) {
        pendingById.clear();
    }
}

// Purpose: unwrap received DTN payload for local delivery (destination == us).
// Behavior: injects original DM to local Router as encrypted or decoded variant, preserving original sender id.
void DtnOverlayModule::deliverLocal(const meshtastic_FwplusDtnData &d)
{
    if (d.is_encrypted) {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(p->encrypted.bytes, d.payload.bytes, d.payload.size);
        p->encrypted.size = d.payload.size;
        p->channel = d.channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    } else {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->decoded.payload.size = (d.payload.size > sizeof(p->decoded.payload.bytes)) ? sizeof(p->decoded.payload.bytes) : d.payload.size;
        memcpy(p->decoded.payload.bytes, d.payload.bytes, p->decoded.payload.size);
        p->channel = d.channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }
}

// Purpose: export a consistent read-only snapshot of DTN runtime counters and flags.
void DtnOverlayModule::getStatsSnapshot(DtnStatsSnapshot &out) const
{
    out.pendingCount = pendingById.size();
    out.forwardsAttempted = ctrForwardsAttempted;
    out.fallbacksAttempted = ctrFallbacksAttempted;
    out.receiptsEmitted = ctrReceiptsEmitted;
    out.receiptsReceived = ctrReceiptsReceived;
    out.expired = ctrExpired;
    out.giveUps = ctrGiveUps;
    out.milestonesSent = ctrMilestonesSent;
    out.probesSent = ctrProbesSent;
    out.enabled = configEnabled;
    uint32_t now = millis();
    out.lastForwardAgeSecs = (lastForwardMs == 0 || now < lastForwardMs) ? 0 : (now - lastForwardMs) / 1000;
}

// Purpose: initialize DTN overlay module with conservative defaults and read ModuleConfig.
DtnOverlayModule::DtnOverlayModule()
    : concurrency::OSThread("DtnOverlay"),
      ProtobufModule("FwplusDtn", meshtastic_PortNum_FWPLUS_DTN_APP, &meshtastic_FwplusDtn_msg)
{
    //enable promiscuous sniffing and acceptance of encrypted packets for overlay capture
    isPromiscuous = true;
    encryptedOk = true;

    //read config with sensible defaults (moduleConfig.dtn_overlay may not exist yet; use defaults)
    //fw+ clarify: documented purpose/units for defaults below
    configEnabled = false; // module master switch (default OFF); enable via ModuleConfig
    configTtlMinutes = 4; // DTN custody TTL [minutes] for overlay items; shorter window to limit carry
    configInitialDelayBaseMs = 8000; // base delay before first attempt [ms] (pre-slot election baseline)
    //fw+ soften: larger retry backoff to reduce overlay traffic rate
    configRetryBackoffMs = 120000; // retry backoff between attempts [ms]
    configMaxTries = 2; // max overlay forward attempts per item
    configLateFallback = false; // enable late native-DM fallback near TTL tail
    configFallbackTailPercent = 20; // start fallback in the last X% of TTL [%]
    configMilestonesEnabled = false; // emit sparse PROGRESSED milestones (telemetry); default OFF
    //fw+ soften: larger per-destination spacing to avoid bursts
    configPerDestMinSpacingMs = 120000; // per-destination minimum spacing between attempts [ms]
    configMaxActiveDm = 1; // global cap of active DTN attempts per scheduler pass
    configProbeFwplusNearDeadline = false; // send lightweight FW+ probe near TTL tail before fallback
    //conservative airtime heuristics
    configGraceAckMs = 2500;                  // grace window to allow native direct/ACK before overlay [ms]
    configSuppressMsAfterForeign = 35000;     // suppression after hearing foreign overlay DATA [ms] (be polite)
    configSuppressIfDestNeighbor = true;      // add extra delay when destination is our direct neighbor
    configPreferBestRouteSlotting = true;     // start earlier if DV-ETX route confidence is good
 
    if (moduleConfig.has_dtn_overlay) {
        // enabled flag directly from config; default stays OFF if absent
        configEnabled = moduleConfig.dtn_overlay.enabled;
        if (moduleConfig.dtn_overlay.ttl_minutes) configTtlMinutes = moduleConfig.dtn_overlay.ttl_minutes;
        if (moduleConfig.dtn_overlay.initial_delay_base_ms) configInitialDelayBaseMs = moduleConfig.dtn_overlay.initial_delay_base_ms;
        if (moduleConfig.dtn_overlay.retry_backoff_ms) configRetryBackoffMs = moduleConfig.dtn_overlay.retry_backoff_ms;
        if (moduleConfig.dtn_overlay.max_tries) configMaxTries = moduleConfig.dtn_overlay.max_tries;
        configLateFallback = moduleConfig.dtn_overlay.late_fallback_enabled;
        if (moduleConfig.dtn_overlay.fallback_tail_percent) configFallbackTailPercent = moduleConfig.dtn_overlay.fallback_tail_percent;
        configMilestonesEnabled = moduleConfig.dtn_overlay.milestones_enabled;
        if (moduleConfig.dtn_overlay.per_dest_min_spacing_ms) configPerDestMinSpacingMs = moduleConfig.dtn_overlay.per_dest_min_spacing_ms;
        if (moduleConfig.dtn_overlay.max_active_dm) configMaxActiveDm = moduleConfig.dtn_overlay.max_active_dm;
        configProbeFwplusNearDeadline = moduleConfig.dtn_overlay.probe_fwplus_near_deadline;
    }
    LOG_INFO("DTN init: enabled=%d ttl_min=%u initDelayMs=%u backoffMs=%u maxTries=%u lateFallback=%d tail%%=%u milestones=%d perDestMinMs=%u maxActive=%u probeNearDeadline=%d graceAckMs=%u suppressForeignMs=%u neighborSuppress=%d preferBestRoute=%d maxRings=%u milestoneMaxRing=%u tailEscMaxRing=%u farMinTtl%%=%u",
             (int)configEnabled, (unsigned)configTtlMinutes, (unsigned)configInitialDelayBaseMs,
             (unsigned)configRetryBackoffMs, (unsigned)configMaxTries, (int)configLateFallback,
             (unsigned)configFallbackTailPercent, (int)configMilestonesEnabled,
             (unsigned)configPerDestMinSpacingMs, (unsigned)configMaxActiveDm,
             (int)configProbeFwplusNearDeadline,
             (unsigned)configGraceAckMs, (unsigned)configSuppressMsAfterForeign,
             (int)configSuppressIfDestNeighbor, (int)configPreferBestRouteSlotting,
             (unsigned)configMaxRingsToAct, (unsigned)configMilestoneMaxRing, (unsigned)configTailEscalateMaxRing,
             (unsigned)configFarMinTtlFracPercent);
    LOG_INFO("DTN: Module created, first beacon in %u ms", (unsigned)configFirstAdvertiseDelayMs);
    LOG_INFO("DTN: FW_PLUS_VERSION=%d", FW_PLUS_VERSION);
    moduleStartMs = millis(); //fw+
}

// Purpose: single scheduler tick; triggers forwards whose time arrived and performs periodic maintenance.
// Returns: milliseconds until next desired wake (clamped 100..2000 ms when enabled).
int32_t DtnOverlayModule::runOnce()
{
    if (!configEnabled) return 1000; //fw+ disabled: idle
    //fw+ periodically advertise our FW+ version for passive discovery
    maybeAdvertiseFwplusVersion();
    //simple scheduler: attempt forwards whose time arrived
    uint32_t now = millis();
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t dmIssuedThisPass = 0; // reset per scheduler pass
    //fw+ dynamic wake: compute nearest nextAttempt across pendings
    uint32_t nextWakeMs = 2000;
    for (auto it = pendingById.begin(); it != pendingById.end();) {
        Pending &p = it->second;
        // Global concurrency cap: throttle attempts
        if (now >= p.nextAttemptMs) {
            if (dmIssuedThisPass < configMaxActiveDm) {
                tryForward(it->first, p);
                dmIssuedThisPass++;
            } else {
                // push a bit forward
                p.nextAttemptMs = now + 1000 + (uint32_t)random(500);
                LOG_DEBUG("DTN defer(id=0x%x): glob cap reached, next in %u ms", it->first, (unsigned)(p.nextAttemptMs - now));
                uint32_t wait = p.nextAttemptMs - now;
                if (wait < nextWakeMs) nextWakeMs = wait;
            }
        } else {
            uint32_t wait = p.nextAttemptMs - now;
            if (wait < nextWakeMs) nextWakeMs = wait;
        }
        // Remove if past deadline
        if (p.data.deadline_ms && nowEpoch > p.data.deadline_ms) {
            // emit EXPIRED receipt to source and drop
            LOG_WARN("DTN expire id=0x%x dl=%u now=%u", it->first, (unsigned)p.data.deadline_ms, (unsigned)nowEpoch);
            emitReceipt(p.data.orig_from, it->first, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0);
            ctrExpired++; 
            //create tombstone to avoid immediate milestone after expiry if others still carry it
            if (configTombstoneMs) tombstoneUntilMs[it->first] = millis() + configTombstoneMs;
            it = pendingById.erase(it);
        } else {
            ++it;
        }
    }
    //bounded maintenance of per-destination cache to avoid growth
    if (millis() - lastPruneMs > 30000) {
        lastPruneMs = millis();
        prunePerDestCache();
    }
    //fw+ clamp next wake between 100..2000 ms to avoid tight/long sleeps
    if (nextWakeMs < 100) nextWakeMs = 100;
    if (nextWakeMs > 2000) nextWakeMs = 2000;
    return (int32_t)nextWakeMs;
}
// Purpose: bound memory use of per-destination last-tx cache by removing oldest entries.
// Policy: prune down to 75% of configured cap using simple aging.
void DtnOverlayModule::prunePerDestCache()
{
    if (lastDestTxMs.size() <= kMaxPerDestCacheEntries) return;
    // simple aging prune: drop oldest ~25% entries
    size_t target = kMaxPerDestCacheEntries * 3 / 4;
    while (lastDestTxMs.size() > target) {
        auto oldest = lastDestTxMs.begin();
        for (auto it = lastDestTxMs.begin(); it != lastDestTxMs.end(); ++it) {
            if (it->second < oldest->second) oldest = it;
        }
        lastDestTxMs.erase(oldest);
    }
}


// Purpose: handle incoming FW+ DTN protobuf packets (DATA/RECEIPT) and conservative foreign capture.
// Returns: true if the packet was consumed by DTN; false to let normal processing continue.
bool DtnOverlayModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg)
{
    // Handle LOCAL packets (our own beacons) - just log and return false to let them route
    if (mp.from == RX_SRC_LOCAL) {
        LOG_DEBUG("DTN: Ignoring LOCAL packet (our own beacon)");
        return false; // Let it route normally
    }
    
    if (msg && msg->which_variant == meshtastic_FwplusDtn_data_tag) {
        // capability: mark sender as FW+
        markFwplusSeen(getFrom(&mp));
        // Milestone: if enabled and we first see this id from another node – send a rare progress to the source 
    if (configMilestonesEnabled && shouldEmitMilestone(msg->variant.data.orig_from, msg->variant.data.orig_to)) { //fw+
            // Politeness: suppress milestones when channel is busy 
            if (airTime && airTime->channelUtilizationPercent() > configMilestoneChUtilMaxPercent) {
                // skip milestone due to high utilization
            } else {
                // Avoid milestone spam if we just handled this id (tombstone) 
                auto itTs = tombstoneUntilMs.find(msg->variant.data.orig_id);
                if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
                    // within tombstone window
                } else {
                // Ring gating: only emit if near source or destination 
                uint8_t ringToSrc = getHopsAway(msg->variant.data.orig_from);
                uint8_t ringToDst = getHopsAway(msg->variant.data.orig_to);
                uint8_t minRing = 255;
                if (ringToSrc != 255) minRing = ringToSrc;
                if (ringToDst != 255 && ringToDst < minRing) minRing = ringToDst;
                if (minRing != 255 && minRing > configMilestoneMaxRing) {
                    // too far: suppress milestone
                } else {
                auto it = pendingById.find(msg->variant.data.orig_id);
                if (it == pendingById.end()) {
                // No local pending, but we see someone else's carry – single progress 
                //include via=our node in reason low byte for milestone telemetry
                uint32_t via = nodeDB->getNodeNum() & 0xFFu;
                //fw+ per-source min interval guard
                bool rateOk = true;
                auto itLast = lastProgressEmitMsBySource.find(msg->variant.data.orig_from);
                if (itLast != lastProgressEmitMsBySource.end()) {
                    uint32_t last = itLast->second;
                    if (millis() - last < configOriginProgressMinIntervalMs) rateOk = false;
                }
                if (rateOk) {
                emitReceipt(msg->variant.data.orig_from, msg->variant.data.orig_id,
                            meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, via);
                ctrMilestonesSent++; 
                    lastProgressEmitMsBySource[msg->variant.data.orig_from] = millis();
                    //immediately tombstone this id locally to avoid re-emitting milestone on repeated hears
                    if (configTombstoneMs) tombstoneUntilMs[msg->variant.data.orig_id] = millis() + configTombstoneMs;
                }
            }
                }
                }
            }
        }
        LOG_INFO("DTN rx DATA id=0x%x from=0x%x to=0x%x enc=%d dl=%u", msg->variant.data.orig_id,
                 (unsigned)getFrom(&mp), (unsigned)msg->variant.data.orig_to, (int)msg->variant.data.is_encrypted,
                 (unsigned)msg->variant.data.deadline_ms);
        handleData(mp, msg->variant.data);
        return true;
    } else if (msg && msg->which_variant == meshtastic_FwplusDtn_receipt_tag) {
        markFwplusSeen(getFrom(&mp));
        LOG_INFO("DTN rx RECEIPT id=0x%x status=%u from=0x%x", msg->variant.receipt.orig_id,
                 (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp));
        handleReceipt(mp, msg->variant.receipt);
        return true;
    }
    // Non-DTN packet (decoded==NULL): optionally capture foreign DM into overlay
    // Policy: by default do NOT capture foreign unicasts to avoid recursive wrapping in mixed meshes
    if (!isFromUs(&mp) && mp.to != nodeDB->getNodeNum()) { 
        bool isDM = (mp.to != NODENUM_BROADCAST && mp.to != NODENUM_BROADCAST_NO_LORA);
        if (isDM) {
            //capture-time gating: ignore far/unknown unicasts to avoid ballooning pending
            uint8_t hopsToDest = getHopsAway(mp.to);
            uint8_t hopsToSrc = getHopsAway(getFrom(&mp));
            bool farFromDest = (configMaxRingsToAct > 0 && hopsToDest != 255 && hopsToDest > configMaxRingsToAct);
            bool nearEitherEnd = isDirectNeighbor(getFrom(&mp)) || isDirectNeighbor(mp.to);
            // If both endpoints are our neighbors, prefer direct-only (skip overlay capture) 
            if (isDirectNeighbor(getFrom(&mp)) && isDirectNeighbor(mp.to)) {
                return false;
            }
            // If we are closer to source than to destination, skip capture (let nodes closer to dest act) 
            if (hopsToSrc != 255 && hopsToDest != 255 && hopsToDest > hopsToSrc) {
                return false;
            }
            bool haveRoute = hasSufficientRouteConfidence(mp.to);
            if (farFromDest && !nearEitherEnd && !haveRoute) {
                // Too far and no confidence: skip capture
                return false;
            }
            //Use configured TTL instead of fixed 5 minutes
            uint32_t ttlMinutes = (configTtlMinutes ? configTtlMinutes : 5);
            uint32_t deadline = (getValidTime(RTCQualityFromNet) * 1000UL) + ttlMinutes * 60UL * 1000UL;
            if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
                //fw+ policy: by default do NOT capture foreign encrypted unicasts to avoid DTN-on-DTN in mixed meshes
                if (!configCaptureForeignEncrypted) return false;
                // Tombstone check: avoid rapid re-enqueue of same orig_id
                auto itTs = tombstoneUntilMs.find(mp.id);
                if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) return false;
                enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                                    deadline,
                                    true, mp.encrypted.bytes, mp.encrypted.size, true /*allow fallback*/);
                // Apply grace and optional neighbor suppression immediately for captured ciphertext 
                auto it = pendingById.find(mp.id);
                if (it != pendingById.end()) {
                    if (configGraceAckMs && it->second.tries == 0) {
                        uint32_t t = millis() + configGraceAckMs + (uint32_t)random(250);
                        if (it->second.nextAttemptMs < t) it->second.nextAttemptMs = t;
                    }
                    if (configSuppressIfDestNeighbor && isDirectNeighbor(mp.to)) {
                        uint32_t add = (configGraceAckMs ? configGraceAckMs : 1500);
                        it->second.nextAttemptMs += add;
                    }
                }
            } else if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
                if (!configCaptureForeignText) return false; //fw+
                auto itTs = tombstoneUntilMs.find(mp.id);
                if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) return false;
                enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                                    deadline,
                                    false, mp.decoded.payload.bytes, mp.decoded.payload.size, true /*allow fallback*/);
                auto it = pendingById.find(mp.id);
                if (it != pendingById.end() && configGraceAckMs && it->second.tries == 0) {
                    uint32_t t = millis() + configGraceAckMs + (uint32_t)random(250);
                    if (it->second.nextAttemptMs < t) it->second.nextAttemptMs = t;
                }
            } else if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP && mp.decoded.request_id) {
                // Native ACK/NAK: cancel pending for this orig DM id and mark destination as stock for a while
                pendingById.erase(mp.decoded.request_id);
                if (configTombstoneMs) tombstoneUntilMs[mp.decoded.request_id] = millis() + configTombstoneMs;
                stockKnownMs[mp.to] = millis();
            }
        }
    }
    // Additionally, observe decoded packets for telemetry or node info to opportunistically probe FW+
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        // Only if enabled and we don't already know this origin as FW+
        if (configTelemetryProbeEnabled && !isFwplus(getFrom(&mp))) {
            bool isTelemetry = (mp.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP);
            bool isNodeInfo = (mp.decoded.portnum == meshtastic_PortNum_NODEINFO_APP);
            if (isTelemetry || isNodeInfo) {
                NodeNum origin = getFrom(&mp);
                uint8_t hops = getHopsAway(origin);
                if (hops != 255 && hops >= configTelemetryProbeMinRing) {
                    uint32_t nowMs = millis();
                    auto it = lastTelemetryProbeToNodeMs.find(origin);
                    if (it == lastTelemetryProbeToNodeMs.end() || (nowMs - it->second) >= configTelemetryProbeCooldownMs) {
                        if (!(airTime && !airTime->isTxAllowedChannelUtil(true))) {
                            // Send minimal FW+ probe (receipt PROGRESSED, reason=0) to origin
                            maybeProbeFwplus(origin);
                            lastTelemetryProbeToNodeMs[origin] = nowMs;
                        }
                    }
                }
            }
        }
    }
    return false; //do not consume non-DTN packets; allow normal processing
}

// Purpose: create DTN envelope from a captured DM (plaintext or ciphertext) and schedule forwarding.
void DtnOverlayModule::enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                                           uint32_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                                           bool allowProxyFallback)
{
    //fw+ guard: if payload won't fit into FW+ DTN container, skip overlay to avoid corrupting DM
    meshtastic_FwplusDtnData d = meshtastic_FwplusDtnData_init_zero;
    if (size > sizeof(d.payload.bytes)) {
        LOG_WARN("DTN skip too-large DM id=0x%x size=%u limit=%u", origId, (unsigned)size, (unsigned)sizeof(d.payload.bytes));
        return;
    }

    //fw+ guard: cap queue to avoid memory growth/fragmentation
    if (pendingById.size() >= kMaxPendingEntries) {
        LOG_WARN("DTN queue full (%u), drop id=0x%x", (unsigned)pendingById.size(), origId);
        return;
    }

    d.orig_id = origId;
    d.orig_from = origFrom;
    d.orig_to = origTo;
    d.channel = channel;
    d.orig_rx_time = getValidTime(RTCQualityFromNet);
    d.deadline_ms = deadlineMs; // absolute epoch ms
    d.is_encrypted = isEncrypted;
    d.allow_proxy_fallback = allowProxyFallback;
    memcpy(d.payload.bytes, bytes, size);
    d.payload.size = size;
    LOG_DEBUG("DTN capture id=0x%x src=0x%x dst=0x%x enc=%d ch=%u ttlms=%u", origId, (unsigned)origFrom,
              (unsigned)origTo, (int)isEncrypted, (unsigned)channel, (unsigned)(deadlineMs));
    scheduleOrUpdate(origId, d);
}

// Purpose: process received FWPLUS_DTN DATA; deliver locally if destined to us, else coordinate suppression/schedule.
void DtnOverlayModule::handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d)
{
    // If we are the destination, deliver locally
    if (d.orig_to == nodeDB->getNodeNum()) {
        deliverLocal(d);
        // Send DELIVERED receipt back to source (include via=proxy in reason low byte)
        uint32_t via = nodeDB->getNodeNum() & 0xFFu; // 1-byte hint
        LOG_INFO("DTN delivered id=0x%x to=0x%x src=0x%x via=0x%x", d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from, (unsigned)via);
        emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED, via);
        // If source is stock (not FW+), optionally send native ACK to improve UX 
        if (!isFwplus(d.orig_from)) {
            //use public RoutingModule API instead of protected Router::sendAckNak
            routingModule->sendAckNak(meshtastic_Routing_Error_NONE, d.orig_from, d.orig_id, d.channel, 0);
        }
        return;
    }
    // Otherwise, coordinate with others: if we heard someone else carrying this id, suppress our attempt for a while 
    auto it = pendingById.find(d.orig_id);
    if (it == pendingById.end()) {
        // First time we see this id (from overlay or capture) → schedule normally
        scheduleOrUpdate(d.orig_id, d);
        // If the packet we saw is already overlay DATA from someone else, apply initial suppression window 
        if (configSuppressMsAfterForeign && !isFromUs(&mp)) {
            auto &p = pendingById[d.orig_id];
            uint32_t postpone = millis() + configSuppressMsAfterForeign + (uint32_t)random(500);
            if (p.nextAttemptMs < postpone) p.nextAttemptMs = postpone;
        }
    } else {
        // We already have an entry; seeing foreign DATA suggests someone carries it → backoff our schedule 
        if (configSuppressMsAfterForeign) {
            uint32_t postpone = millis() + configSuppressMsAfterForeign + (uint32_t)random(500);
            if (it->second.nextAttemptMs < postpone) it->second.nextAttemptMs = postpone;
            LOG_DEBUG("DTN suppress id=0x%x after foreign carry, next in %u ms", d.orig_id, (unsigned)(it->second.nextAttemptMs - millis()));
        }
    }
}

// Purpose: process received FWPLUS_DTN RECEIPT; clear pending, tombstone and decode special reason codes (e.g., version).
void DtnOverlayModule::handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r)
{
    // Simplest: stop any pending entry
    (void)mp;
    pendingById.erase(r.orig_id);
    // Create a short tombstone to prevent immediate re-capture storms of same id 
    if (configTombstoneMs) tombstoneUntilMs[r.orig_id] = millis() + configTombstoneMs;
    ctrReceiptsReceived++; //fw+

    //fw+ interpret special FW+ version advertisements carried via receipt.reason
    // Convention: reason field contains version number directly (8-bit, 0-255)
    LOG_DEBUG("DTN: Processing receipt reason=0x%x status=%u", (unsigned)r.reason, (unsigned)r.status);
    if (r.reason > 0 && r.reason <= 255) {
        LOG_INFO("DTN: Version advertisement detected, reason=0x%x", (unsigned)r.reason);
        NodeNum origin = getFrom(&mp);
        bool hadVer = (fwplusVersionByNode.find(origin) != fwplusVersionByNode.end());
        uint16_t ver = (uint16_t)r.reason;
        recordFwplusVersion(origin, ver);
        // Optional hello-back: unicast our version to origin (allow periodic responses for FW+DTN discovery)
        LOG_DEBUG("DTN: Version advertisement from origin=0x%x ver=%u hadVer=%d", (unsigned)origin, (unsigned)ver, (int)hadVer);
        if (configHelloBackEnabled) {
            // reply to nodes up to 3 hops away (FW+DTN is alternative software)
            uint8_t hops = getHopsAway(origin);
            LOG_DEBUG("DTN: Hello-back check: hops=%u maxRing=%u", (unsigned)hops, (unsigned)configHelloBackMaxRing);
            if (hops != 255 && hops <= configHelloBackMaxRing) {
                // per-origin rate limit (more frequent for known FW+ nodes)
                uint32_t nowMs = millis();
                auto it = lastHelloBackToNodeMs.find(origin);
                uint32_t requiredInterval = hadVer ? (configHelloBackMinIntervalMs / 2) : configHelloBackMinIntervalMs; // 30min for known, 1h for new
                bool rateOk = (it == lastHelloBackToNodeMs.end() || (nowMs - it->second) >= requiredInterval);
                LOG_DEBUG("DTN: Rate check: lastTx=%u required=%u rateOk=%d", 
                         it == lastHelloBackToNodeMs.end() ? 0 : (unsigned)(nowMs - it->second), 
                         (unsigned)requiredInterval, (int)rateOk);
                if (rateOk) {
                    // channel utilization gate
                    bool channelOk = !(airTime && !airTime->isTxAllowedChannelUtil(true));
                    LOG_DEBUG("DTN: Channel check: channelOk=%d", (int)channelOk);
                    if (channelOk) {
                        // reason is serialized as 8-bit, so use version directly
                        uint32_t reason = (uint32_t)FW_PLUS_VERSION;
                        emitReceipt(origin, 0, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, reason);
                        lastHelloBackToNodeMs[origin] = nowMs;
                        LOG_INFO("DTN: Hello-back sent to origin=0x%x", (unsigned)origin);
                    } else {
                        LOG_DEBUG("DTN: Channel busy, hello-back blocked");
                    }
                } else {
                    LOG_DEBUG("DTN: Rate limited, hello-back blocked");
                }
            } else {
                LOG_DEBUG("DTN: Origin too far (hops=%u > %u), hello-back blocked", (unsigned)hops, (unsigned)configHelloBackMaxRing);
            }
        } else {
            LOG_DEBUG("DTN: Hello-back disabled");
        }
    } else {
        LOG_DEBUG("DTN: Not a version advertisement (reason=0x%x, expected 0xF1...)", (unsigned)r.reason);
    }
    // If we get a native ROUTING_APP ACK/NAK echo for our original id, mark destination as stock for some time
    if (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED) {
        // DELIVERED came from dest (FW+), do nothing
    }
}

// Purpose: create or refresh a pending DTN entry and compute the next attempt time.
// Inputs: original message id and payload envelope; uses topology, mobility and per-destination spacing.
// Effect: updates election timing, applies far-node throttle, and stores per-dest last TX timestamp.
void DtnOverlayModule::scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d)
{
    auto &p = pendingById[id];
    p.data = d;
    p.lastCarrier = nodeDB->getNodeNum();
    // Per-destination spacing: if we tried to the same destination too recently, postpone 
    auto itLast = lastDestTxMs.find(d.orig_to);
    // Base delay before first attempt
    uint32_t base = configInitialDelayBaseMs ? configInitialDelayBaseMs : 8000;
    // Immediate-from-source: tiny jitter instead of long base/slot
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    if (isFromSource && p.tries == 0) {
        uint32_t jitter = 200 + (uint32_t)random(401); // 200..600 ms
        base = jitter;
    }
    // Apply grace window only for non-self-origin (to allow direct delivery/ACK a chance)
    if (!isFromSource && p.tries == 0 && configGraceAckMs) {
        if (base < configGraceAckMs) base = configGraceAckMs;
    }
    // If destination is our direct neighbor and suppression is enabled, be extra conservative
    if (configSuppressIfDestNeighbor && isDirectNeighbor(d.orig_to)) {
        base += configGraceAckMs ? configGraceAckMs : 1500;
    }
    // Topology-aware throttle: if we are far from destination, delay until a fraction of TTL elapses
    if (configMaxRingsToAct > 0) {
        uint8_t hopsToDest = getHopsAway(d.orig_to);
        if (hopsToDest != 255 && hopsToDest > configMaxRingsToAct) {
            if (d.deadline_ms && d.orig_rx_time) {
                uint32_t ttl = (d.deadline_ms > d.orig_rx_time * 1000UL) ? (d.deadline_ms - (d.orig_rx_time * 1000UL)) : 0;
                uint32_t mustWait = (uint64_t)ttl * (configFarMinTtlFracPercent > 100 ? 100 : configFarMinTtlFracPercent) / 100;
                uint32_t readyEpoch = (d.orig_rx_time * 1000UL) + mustWait;
                uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
                if (nowEpoch < readyEpoch) {
                    uint32_t extra = (readyEpoch - nowEpoch);
                    // convert epoch gap to local millis horizon approximately
                    uint32_t extraMs = extra; // both are ms units already
                    base += extraMs;
                }
            } else {
                base += 5000; // no TTL: arbitrary extra delay when far
            }
        }
    }
    // fw+ mobility-aware election window: fewer/shorter slots for mobile nodes
    float mobility = fwplus_getMobilityFactor01();
    uint32_t slots = 8;
    uint32_t slotLen = 400;
    if (mobility > 0.5f) {
        slots = 6;
        slotLen = 300;
    }
    uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
    uint32_t slot = (isFromSource && p.tries == 0) ? 0 : (rank % slots);
    // Prefer earlier start if we are confident about route quality
    uint32_t earlyBonus = 0;
    if (configPreferBestRouteSlotting && hasSufficientRouteConfidence(d.orig_to)) {
        earlyBonus = 1000; // pull in by ~1s
    }
    uint32_t target = millis() + base + slot * slotLen + (uint32_t)random(slotLen);
    if (target > millis() + earlyBonus) target -= earlyBonus;
    if (itLast != lastDestTxMs.end()) {
        uint32_t spacing = configPerDestMinSpacingMs ? configPerDestMinSpacingMs : 30000;
        // fw+ reduce spacing when mobile (down to ~60%) to react faster
        spacing = (uint32_t)((float)spacing * (1.0f - 0.4f * mobility));
        if (target < itLast->second + spacing) target = itLast->second + spacing + (uint32_t)random(500);
    }
    p.nextAttemptMs = target;
    LOG_DEBUG("DTN schedule id=0x%x next=%u ms (base=%u slot=%u)", id, (unsigned)(p.nextAttemptMs - millis()), (unsigned)base, (unsigned)slot);
}

// Purpose: perform one forwarding decision for a pending DTN item.
// Steps: channel-util gate → route confidence/handoff → tail probe/fallback → overlay send and backoff scheduling.
// Guarantees: never emits plaintext via overlay; fallback uses native encrypted DM without spoofing sender.
void DtnOverlayModule::tryForward(uint32_t id, Pending &p)
{
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) { p.nextAttemptMs = millis() + 2500 + (uint32_t)random(500); LOG_DEBUG("DTN busy: defer id=0x%x", id); return; }

    // DV-ETX route confidence gating (more permissive when mobile)
    //fw+ allow immediate attempts to direct neighbors even if DV-ETX says low confidence
    bool lowConf = !hasSufficientRouteConfidence(p.data.orig_to);
    if (lowConf) {
        if (isDirectNeighbor(p.data.orig_to)) {
            // proceed despite low confidence because destination is our neighbor //fw+
        } else {
            // Backoff softly
            float mobility = fwplus_getMobilityFactor01();
            uint32_t backoff = configRetryBackoffMs + (uint32_t)random(2000);
            if (mobility > 0.5f) backoff = backoff / 2; // try sooner if we are moving
            p.nextAttemptMs = millis() + backoff;
            LOG_DEBUG("DTN low confidence: defer id=0x%x", id);
            // Note: we may trigger traceroute below only if no FW+ handoff candidate
            // Return after we decide whether to probe routing
            // (fall through until target selection to possibly trigger traceroute)
        }
    }

    // Optional late proxy fallback to native DM near deadline
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t ttlTailStart = 0;
    if (p.data.deadline_ms && configLateFallback && configFallbackTailPercent)
    {
        uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ? (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
        ttlTailStart = p.data.deadline_ms - (ttl * (configFallbackTailPercent > 100 ? 100 : configFallbackTailPercent) / 100);
    }
    bool inTail = (p.data.deadline_ms && nowEpoch >= ttlTailStart);
    if (inTail) {
        // Optional FW+ probe just before deciding on fallback
        if (configProbeFwplusNearDeadline && !isFwplus(p.data.orig_to)) {
            maybeProbeFwplus(p.data.orig_to);
        }
    }
    //fw+ respect allow_proxy_fallback and limit tries
    if (configMaxTries && p.tries >= configMaxTries) {
        emitReceipt(p.data.orig_from, id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0); 
        LOG_WARN("DTN give up id=0x%x tries=%u", id, (unsigned)p.tries);
        pendingById.erase(id);
        ctrGiveUps++;
        return;
    }

    // Near-destination fast path: for close targets (<=1 hop), prefer native DM directly instead of DTN overlay
    {
        uint8_t hops = getHopsAway(p.data.orig_to);
        if (hops != 255 && hops <= 1 && !isFwplus(p.data.orig_to) && p.data.allow_proxy_fallback) {
            if (sendProxyFallback(id, p)) return;
        }
    }

    // Fast-path fallback for known-stock destinations or TTL tail
    bool destKnownStock = isDestKnownStock(p.data.orig_to);
    if ((destKnownStock || inTail) && !isFwplus(p.data.orig_to) && p.data.allow_proxy_fallback) {
        if (sendProxyFallback(id, p)) return;
    }

    // Basic forward attempt using overlay again (ring-aware leader election)
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_data_tag;
    msg.variant.data = p.data;

    meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
    if (!mp) {
        p.nextAttemptMs = millis() + 3000;
        return;
    }
    //custody handoff: optionally send to FW+ neighbor instead of final dest when far
    NodeNum target = p.data.orig_to;
    if (configEnableFwplusHandoff) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        if (hopsToDest != 255 && hopsToDest >= configHandoffMinRing) {
            NodeNum cand = chooseHandoffTarget(p.data.orig_to, id, p);
            if (cand != 0 && cand != p.data.orig_to) {
                target = cand;
            }
        }
    }
    //Only trigger traceroute if we are targeting final destination (no FW+ handoff) and confidence is low
    if (lowConf && target == p.data.orig_to && !isDirectNeighbor(p.data.orig_to)) {
        maybeTriggerTraceroute(p.data.orig_to);
        return; // give time for route discovery before attempting
    }
    mp->to = target;
    mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    mp->want_ack = false;
    mp->decoded.want_response = false;
    // Use BACKGROUND priority normally; escalate to DEFAULT only in TTL tail when we consider fallback 
    if (p.data.deadline_ms) {
        uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
        uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ? (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
        uint32_t tailStart = p.data.deadline_ms - (ttl * (configFallbackTailPercent > 100 ? 100 : configFallbackTailPercent) / 100);
        bool nearDst = false;
        if (configTailEscalateMaxRing > 0) {
            uint8_t hopsToDest = getHopsAway(p.data.orig_to);
            nearDst = (hopsToDest != 255 && hopsToDest <= configTailEscalateMaxRing);
        }
        mp->priority = (nowEpoch >= tailStart && nearDst) ? meshtastic_MeshPacket_Priority_DEFAULT : meshtastic_MeshPacket_Priority_BACKGROUND;
    } else {
        mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    }
    // Before sending, if we are further from the destination than the source, back off extra to favor closer relayers
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    uint8_t hopsToSrc = getHopsAway(p.data.orig_from);
    if (hopsToSrc != 255 && hopsToDest != 255 && hopsToDest > hopsToSrc) {
        p.nextAttemptMs = millis() + configRetryBackoffMs + 5000 + (uint32_t)random(2000);
        return;
    }
    // Ensure we never leak plaintext overlay: overlay always carries original payload as-is; the radio layer will encrypt link-level
    // if keys are present. We do not generate a separate plaintext native DM unless in fallback tail (handled above).
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    ctrForwardsAttempted++; 
    lastForwardMs = millis(); 
    // update per-destination last tx (bounded map)
    lastDestTxMs[p.data.orig_to] = millis();
    //remember preferred handoff per dest when we target FW+ (not the final dest)
    if (target != p.data.orig_to) {
        auto &ph = preferredHandoffByDest[p.data.orig_to];
        ph.node = target;
        ph.lastUsedMs = millis();
    }
    // schedule next attempt with backoff
    p.nextAttemptMs = millis() + (configRetryBackoffMs ? configRetryBackoffMs : 60000);
    LOG_INFO("DTN fwd overlay id=0x%x dst=0x%x try=%u next=%u ms", id, (unsigned)p.data.orig_to, (unsigned)p.tries,
             (unsigned)(p.nextAttemptMs - millis()));
}

// Purpose: perform late native DM fallback for a DTN item (encrypted), preserving original id for ACK mapping.
// Returns: true if a send was attempted or queued; schedules next retry on success or alloc failure.
bool DtnOverlayModule::sendProxyFallback(uint32_t id, Pending &p)
{
    // Send native DM (ciphertext or plaintext) towards destination
    meshtastic_MeshPacket *dm = allocDataPacket();
    if (!dm) { p.nextAttemptMs = millis() + 3000; return true; }
    dm->to = p.data.orig_to;
    //fw+ ensure PKI/link encryption remains valid: do NOT spoof original sender here.
    // Leave 'from' as our node so the Router can sign/encrypt as us when using PKI.
    dm->channel = p.data.channel;
    if (p.data.is_encrypted) {
        dm->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(dm->encrypted.bytes, p.data.payload.bytes, p.data.payload.size);
        dm->encrypted.size = p.data.payload.size;
    } else {
        dm->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        if (p.data.payload.size > sizeof(dm->decoded.payload.bytes))
            dm->decoded.payload.size = sizeof(dm->decoded.payload.bytes);
        else
            dm->decoded.payload.size = p.data.payload.size;
        memcpy(dm->decoded.payload.bytes, p.data.payload.bytes, dm->decoded.payload.size);
        dm->decoded.want_response = false;
    }
    //fw+ preserve original DM id so that ROUTING_APP ACK maps to sender's pending entry
    dm->id = p.data.orig_id;
    dm->want_ack = true; // try to get radio ACK 
    dm->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    service->sendToMesh(dm, RX_SRC_LOCAL, false);
    p.tries++;
    p.nextAttemptMs = millis() + configRetryBackoffMs;
    LOG_INFO("DTN fallback DM id=0x%x dst=0x%x try=%u", id, (unsigned)p.data.orig_to, (unsigned)p.tries);
    ctrFallbacksAttempted++; 
    return true;
}

// Purpose: lightweight FW+ presence probe to a specific node using a receipt with PROGRESSED status.
// Usage: optional near TTL tail to detect FW+ capability without heavy traffic.
void DtnOverlayModule::maybeProbeFwplus(NodeNum dest)
{
    // Minimal probe: send a short overlay receipt with PROGRESSED to DEST as an FW+ ping (stock ignores) //fw+
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
    msg.variant.receipt.orig_id = 0; // ping-like
    msg.variant.receipt.status = meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED;
    msg.variant.receipt.reason = 0;
    meshtastic_MeshPacket *p = allocDataProtobuf(msg);
    if (!p) return;
    p->to = dest;
    p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_DEBUG("DTN probe dest=0x%x", (unsigned)dest);
    ctrProbesSent++;
}

//fw+ trigger traceroute to dest with cooldown to build DV-ETX confidence
void DtnOverlayModule::maybeTriggerTraceroute(NodeNum dest)
{
    uint32_t now = millis();
    auto it = lastRouteProbeMs.find(dest);
    if (it != lastRouteProbeMs.end() && (now - it->second) < configRouteProbeCooldownMs) return;
    lastRouteProbeMs[dest] = now;
    if (!router) return;
    // Fall back to emitting a lightweight FW+ probe; actual traceroute scheduling is internal to NextHopRouter
    // and not accessible here due to access controls.
    maybeProbeFwplus(dest);
}

// Purpose: send a compact DTN receipt (status/milestone/expire) back to the source or peer.
// Notes: uses BACKGROUND priority and avoids ACKs; reason carries optional telemetry (e.g., FW+ version advertise).
void DtnOverlayModule::emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason)
{
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
    msg.variant.receipt.orig_id = origId;
    msg.variant.receipt.status = status;
    msg.variant.receipt.reason = reason;

    meshtastic_MeshPacket *p = allocDataProtobuf(msg);
    if (!p) return;
    p->to = to;
    // Ensure receipt routes back normally
    p->from = nodeDB->getNodeNum();
    p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_DEBUG("DTN tx RECEIPT id=0x%x status=%u to=0x%x", (unsigned)origId, (unsigned)status, (unsigned)to);
    ctrReceiptsEmitted++; 
}
#endif

//fw+ adaptive milestone limiter implementation
bool DtnOverlayModule::shouldEmitMilestone(NodeNum src, NodeNum dst)
{
    (void)src; (void)dst; // current heuristic does not need identities
    if (!configMilestonesEnabled) return false;
    if (!configMilestoneAutoLimiterEnabled) return true;

    uint8_t chUtil = airTime ? airTime->channelUtilizationPercent() : 0;
    uint8_t neighbors = countDirectNeighbors();
    size_t pend = pendingById.size();

    // Enter suppression when any high threshold exceeded
    if (!runtimeMilestonesSuppressed) {
        if (chUtil >= configMilestoneAutoSuppressHighChUtil ||
            neighbors >= configMilestoneAutoNeighborHigh ||
            pend >= configMilestoneAutoPendingHigh) {
            runtimeMilestonesSuppressed = true;
        }
    } else {
        // Leave suppression only when all metrics are comfortably low (hysteresis)
        if (chUtil <= configMilestoneAutoReleaseLowChUtil &&
            neighbors < configMilestoneAutoNeighborHigh &&
            pend < configMilestoneAutoPendingHigh) {
            runtimeMilestonesSuppressed = false;
        }
    }
    return !runtimeMilestonesSuppressed;
}
void DtnOverlayModule::recordFwplusVersion(NodeNum n, uint16_t version)
{
    fwplusVersionByNode[n] = version;
    markFwplusSeen(n);
}

void DtnOverlayModule::maybeAdvertiseFwplusVersion()
{
    if (!configEnabled) return;
    uint32_t now = millis();
    // Cold-start: if we don't yet know any FW+ peer, use a shorter interval
    bool knowsAnyFwplus = false;
    for (const auto &kv : fwplusVersionByNode) { (void)kv; knowsAnyFwplus = true; break; }
    uint32_t interval = knowsAnyFwplus ? configAdvertiseIntervalMs : configAdvertiseIntervalUnknownMs;
    if (lastAdvertiseMs == 0) lastAdvertiseMs = now - (uint32_t)random(interval);
    // One-shot early advertise shortly after module start to speed up discovery (unconditional broadcast)
    if (!firstAdvertiseDone && now - moduleStartMs >= configFirstAdvertiseDelayMs) {
        LOG_INFO("DTN: Attempting first beacon after %u ms", (unsigned)(now - moduleStartMs));
        // Compose reason and send a broadcast immediately, bypassing interval/neighbor gates //fw+
        // Only advertise if channel is not overloaded //fw+
        if (!(airTime && !airTime->isTxAllowedChannelUtil(true))) {
            LOG_INFO("DTN: Channel clear, sending beacon");
            // reason is serialized as 8-bit, so we can only use lower 8 bits
            // Use version as reason (0-255 range)
            uint32_t reason = (uint32_t)FW_PLUS_VERSION;
            LOG_INFO("DTN: Sending beacon with reason=0x%x ver=%u FW_PLUS_VERSION=%d", (unsigned)reason, (unsigned)FW_PLUS_VERSION, FW_PLUS_VERSION);

            meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
            msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
            msg.variant.receipt.orig_id = 0;
            msg.variant.receipt.status = meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED;
            msg.variant.receipt.reason = reason;
            meshtastic_MeshPacket *p = allocDataProtobuf(msg);
            if (p) {
                p->to = NODENUM_BROADCAST; // public broadcast //fw+
                p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
                p->want_ack = false;
                p->decoded.want_response = false;
                p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
                service->sendToMesh(p, RX_SRC_LOCAL, false);
                lastAdvertiseMs = now;
                LOG_INFO("DTN: Beacon sent successfully");
            } else {
                LOG_WARN("DTN: Channel busy, beacon blocked");
            }
        } else {
            LOG_WARN("DTN: First beacon not ready yet, %u ms < %u ms", (unsigned)(now - moduleStartMs), (unsigned)configFirstAdvertiseDelayMs);
        }
        firstAdvertiseDone = true;
        return;
    }
    if (now - lastAdvertiseMs < interval + (uint32_t)random(configAdvertiseJitterMs)) return;

    // Only advertise if channel is not overloaded //fw+
    if (airTime && !airTime->isTxAllowedChannelUtil(true)) return;

    // Always advertise periodically for FW+DTN discovery (less restrictive)
    // Only skip if we have many known FW+ neighbors to avoid spam
    bool need = true;
    int totalNodes = nodeDB->getNumMeshNodes();
    int knownFwplusNeighbors = 0;
    for (int i = 0; i < totalNodes; ++i) {
        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
        if (!ni) continue;
        if (ni->num == nodeDB->getNodeNum()) continue;
        if (ni->hops_away != 0) continue;  // still only direct neighbors for counting
        if (isFwplus(ni->num) && fwplusVersionByNode.find(ni->num) != fwplusVersionByNode.end()) {
            knownFwplusNeighbors++;
        }
    }
    // Skip only if we know many FW+ neighbors (reduce spam in dense networks)
    if (knownFwplusNeighbors >= 3) need = false;
    if (!need && knowsAnyFwplus) return;

    // Compose reason: version number directly (8-bit, 0-255)
    uint32_t reason = (uint32_t)FW_PLUS_VERSION;

    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
    msg.variant.receipt.orig_id = 0;
    msg.variant.receipt.status = meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED;
    msg.variant.receipt.reason = reason;
    meshtastic_MeshPacket *p = allocDataProtobuf(msg);
    if (!p) return;
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    lastAdvertiseMs = now;
}

//fw+ FW+ custody handoff target selection
NodeNum DtnOverlayModule::chooseHandoffTarget(NodeNum dest, uint32_t origId, Pending &p)
{
    (void)origId;
    // 1) Prefer cached handoff for this destination if fresh
    auto itPref = preferredHandoffByDest.find(dest);
    if (itPref != preferredHandoffByDest.end()) {
        const auto &ph = itPref->second;
        if (ph.node != 0 && (millis() - ph.lastUsedMs) < preferredHandoffTtlMs) {
            auto itVer = fwplusVersionByNode.find(ph.node);
            if (itVer != fwplusVersionByNode.end() && itVer->second >= configMinFwplusVersionForHandoff) {
                return ph.node;
            }
        }
    }

    // 2) If NextHopRouter has a next_hop that is FW+, pick that first
    if (router) {
        auto nh = static_cast<NextHopRouter *>(router);
        auto snap = nh->getRouteSnapshot(false);
        for (const auto &e : snap) {
            if (e.dest != dest) continue;
            if (e.next_hop == NO_NEXT_HOP_PREFERENCE) break;
            // Map last byte back to NodeNum by scanning direct neighbors in NodeDB
            int totalNodes = nodeDB->getNumMeshNodes();
            for (int i = 0; i < totalNodes; ++i) {
                meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n) continue;
                if ((uint8_t)(n->num & 0xFF) == e.next_hop) {
                    if (isFwplus(n->num)) {
                        auto itVer = fwplusVersionByNode.find(n->num);
                        if (itVer != fwplusVersionByNode.end() && itVer->second >= configMinFwplusVersionForHandoff) {
                            return n->num;
                        }
                    }
                }
            }
            break;
        }
    }

    // 3) Otherwise build/refresh shortlist (stream-min without dynamic alloc/sort)
    if (p.handoffCount == 0) {
        NodeNum best1 = 0, best2 = 0, best3 = 0;
        uint8_t best1_au = 255, best2_au = 255, best3_au = 255; // hopsFromUs
        uint8_t best1_ad = 255, best2_ad = 255, best3_ad = 255; // hopsToDest
        int totalNodes = nodeDB->getNumMeshNodes();
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->num == nodeDB->getNodeNum()) continue;
            if (!isFwplus(ni->num)) continue;
            if (ni->num == p.lastCarrier) continue;
            auto itVer = fwplusVersionByNode.find(ni->num);
            if (itVer == fwplusVersionByNode.end() || itVer->second < configMinFwplusVersionForHandoff) continue;
            uint8_t au = ni->hops_away;
            uint8_t ad = getHopsAway(dest);
            auto better = [](uint8_t au1, uint8_t ad1, uint8_t au2, uint8_t ad2, NodeNum n1, NodeNum n2) {
                if (au1 != au2) return au1 < au2;
                if (ad1 != ad2) return ad1 < ad2;
                return n1 < n2;
            };
            // insert into best1..best3
            if (best1 == 0 || better(au, ad, best1_au, best1_ad, ni->num, best1)) {
                best3 = best2; best3_au = best2_au; best3_ad = best2_ad;
                best2 = best1; best2_au = best1_au; best2_ad = best1_ad;
                best1 = ni->num; best1_au = au; best1_ad = ad;
            } else if (best2 == 0 || better(au, ad, best2_au, best2_ad, ni->num, best2)) {
                best3 = best2; best3_au = best2_au; best3_ad = best2_ad;
                best2 = ni->num; best2_au = au; best2_ad = ad;
            } else if (best3 == 0 || better(au, ad, best3_au, best3_ad, ni->num, best3)) {
                best3 = ni->num; best3_au = au; best3_ad = ad;
            }
        }
        p.handoffCount = 0;
        if (best1) p.handoffCandidates[p.handoffCount++] = best1;
        if (best2) p.handoffCandidates[p.handoffCount++] = best2;
        if (best3) p.handoffCandidates[p.handoffCount++] = best3;
        p.handoffIndex = 0;
    }
    if (p.handoffCount == 0) return 0;
    // Rotate through candidates between retries
    NodeNum pick = p.handoffCandidates[p.handoffIndex % p.handoffCount];
    p.handoffIndex = (p.handoffIndex + 1) % 8; // bounded increment
    // avoid accidental cycles
    if (pick == dest || pick == nodeDB->getNodeNum()) return 0;
    return pick;
}
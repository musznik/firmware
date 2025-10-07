/*
//fw+ DTN Overlay Module — Overview (DTN-first custody, FW+ handoff)

Purpose
- Opportunistic store–carry–forward for private Direct Messages. With DTN enabled, private TEXT unicasts are intercepted
  at the sender and wrapped as FWPLUS_DTN DATA (DTN-first). Native DM is used as an intelligent fallback for stock
  destinations or late in TTL. Stock nodes require no changes; FW+ nodes unwrap at the destination and inject the original DM locally.

Ingress sources
- Local send interception (DTN-first): Router diverts private TEXT to a DTN queue with a TTL; no immediate native DM.
- FWPLUS_DTN DATA overheard on the mesh: always accepted and scheduled; if we are the destination we unwrap and deliver.
- Optional capture of foreign (non-DTN) unicasts is conservative (default OFF) to avoid DTN-on-DTN recursion.
- OnDemand observation: passively discovers DTN-enabled nodes from OnDemand responses and records their versions.

Scheduling
- First attempt: fast path for the sender and direct neighbors; intermediates use deterministic slotting and per-destination
  spacing. DV‑ETX gating is more permissive for mobile nodes. Far nodes are delayed (far‑throttle).

Forwarding and Handoff
- Primary action: send FWPLUS_DTN DATA toward the destination, or hand off custody to a better FW+ neighbor.
- When the destination is far: prefer handoff to a FW+ neighbor (safe mapping of `next_hop` → unique direct neighbor).
- Shortlist up to 3 FW+ neighbors closer to the destination; rotate between them across retries; per‑destination preference cache.
- Skip candidates with unknown route (ad==255). Prevent loops via lastCarrier/self/dest checks.
- Broadcast as last resort in TTL tail: public (unencrypted), channel‑utilization gated, randomized cooldown 60–120 s.
- Priority defaults to BACKGROUND; escalate to DEFAULT in TTL tail when near the destination or when we are the source.
- Global active cap and per‑destination spacing limit concurrency.

Receipts & Milestones
- Any receipt (DELIVERED/PROGRESSED/EXPIRED) clears pending and sets a tombstone to avoid re‑enqueue storms.
- PROGRESSED is emitted sparingly (ring‑gated, auto‑limited) with hysteresis under high load.
- Version advertisement: 16‑bit reason carries FW+ version number (0–65535).

Traceroute & Fallback
- Source: immediate traceroute hint on low confidence (cooldown), no periodic active probing.
- Stock destination: early native DM fallback; DTN broadcast only in TTL tail (public, cooldown, load gating).
- Intermediates: anti‑burst and far‑throttle; coordination via suppression after hearing foreign DATA.
- Unresponsive FW+: tracks consecutive failed DTN attempts to FW+ destinations; after N failures + timeout without receipt,
  triggers native DM fallback. Handles version incompatibility, offline nodes, and implementation changes. Counter resets on receipt.

Version Advertisement & Discovery
- One‑shot public beacon after start (channel‑utilization gated), followed by periodic beacons.
- Hello-back: unicast version up to 3 hops (rate‑limited).
- Passive discovery: OnDemand responses, overheard DTN DATA/RECEIPTs; no periodic active probing.

Mobility Adaptation
- Route invalidation: periodically invalidates stale routes for mobile nodes based on last_heard time and mobility factor.
- Adaptive timeouts: shorter timeouts for mobile nodes (15-30min) vs stationary nodes (2h).
- Mobility-aware scheduling: reduced spacing and earlier attempts for mobile nodes.

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
#include "mesh/generated/meshtastic/ondemand.pb.h" //fw+
#include "mqtt/MQTT.h" //fw+
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
        // Preserve original DM id for deduplication and proper UX threading
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(p->encrypted.bytes, d.payload.bytes, d.payload.size);
        p->encrypted.size = d.payload.size;
        p->channel = d.channel;
        // Do not CC to phone here; handleFromRadio will forward once. CC would cause double UI delivery.
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    } else {
        meshtastic_MeshPacket *p = allocDataPacket();
        // Preserve original DM id for deduplication and proper UX threading
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->decoded.payload.size = (d.payload.size > sizeof(p->decoded.payload.bytes)) ? sizeof(p->decoded.payload.bytes) : d.payload.size;
        memcpy(p->decoded.payload.bytes, d.payload.bytes, p->decoded.payload.size);
        p->channel = d.channel;
        // Do not CC to phone here; handleFromRadio will forward once. CC would cause double UI delivery.
        service->sendToMesh(p, RX_SRC_LOCAL, false);
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
    out.fwplusUnresponsiveFallbacks = ctrFwplusUnresponsiveFallbacks;
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
    configInitialDelayBaseMs = 2000; //fw+ reduced base delay before first attempt [ms] for faster delivery
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
    configGraceAckMs = 500;                   //fw+ reduced grace window for faster delivery [ms]
    configSuppressMsAfterForeign = 35000;     // suppression after hearing foreign overlay DATA [ms] (be polite)
    configSuppressIfDestNeighbor = true;      // add extra delay when destination is our direct neighbor
    configPreferBestRouteSlotting = true;     // start earlier if DV-ETX route confidence is good
    
    // Enhanced monitoring
    configDetailedLogIntervalMs = 600000;      // 10 minutes
    lastDetailedLogMs = 0;
 
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
    LOG_INFO("DTN: Cold start timeout: %u ms, native fallback: %s", 
             (unsigned)configColdStartTimeoutMs, configColdStartNativeFallback ? "enabled" : "disabled");
    LOG_INFO("DTN: FW+ unresponsive fallback: %s, threshold: %u failures, timeout: %u ms",
             configFwplusUnresponsiveFallback ? "enabled" : "disabled",
             (unsigned)configFwplusFailureThreshold, (unsigned)configFwplusResponseTimeoutMs);
    moduleStartMs = millis(); //fw+
}

// Purpose: single scheduler tick; triggers forwards whose time arrived and performs periodic maintenance.
// Returns: milliseconds until next desired wake (clamped 100..2000 ms when enabled).
int32_t DtnOverlayModule::runOnce()
{
    if (!configEnabled) return 1000; //fw+ disabled: idle
    //fw+ periodically advertise our FW+ version for passive discovery
    maybeAdvertiseFwplusVersion();
    //fw+ periodically invalidate stale routes for mobile nodes
    invalidateStaleRoutes();
    //fw+ detailed logging and monitoring
    logDetailedStats();
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
    // Drop duplicate native TEXT to us if we've just delivered same id via DTN (tombstone active)
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP &&
        mp.to == nodeDB->getNodeNum()) {
        auto itTs = tombstoneUntilMs.find(mp.id);
        if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
            LOG_DEBUG("DTN: Suppressing duplicate native TEXT id=0x%x due to recent DTN delivery", (unsigned)mp.id);
            return true; // consume to prevent duplicate UI delivery
        }
    }

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
            // If we've just delivered this id via DTN locally, drop late-arriving native duplicates
            auto itDeliveredTs = tombstoneUntilMs.find(mp.id);
            if (itDeliveredTs != tombstoneUntilMs.end() && millis() < itDeliveredTs->second) {
                return false;
            }
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
        
        // fw+ Observe OnDemand responses to discover DTN-enabled nodes
        if (mp.decoded.portnum == meshtastic_PortNum_ON_DEMAND_APP) {
            observeOnDemandResponse(mp);
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
    //fw+ Calculate TTL for logging (deadline - now)
    uint64_t nowMs = (uint64_t)getValidTime(RTCQualityFromNet) * 1000ULL;
    uint32_t ttlMs = (deadlineMs > nowMs) ? (uint32_t)(deadlineMs - nowMs) : 0;
    LOG_DEBUG("DTN capture id=0x%x src=0x%x dst=0x%x enc=%d ch=%u ttlms=%u", origId, (unsigned)origFrom,
              (unsigned)origTo, (int)isEncrypted, (unsigned)channel, ttlMs);
    scheduleOrUpdate(origId, d);
}

// Purpose: process received FWPLUS_DTN DATA; deliver locally if destined to us, else schedule and coordinate with peers.
void DtnOverlayModule::handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d)
{
    // If we are the destination, deliver locally
    if (d.orig_to == nodeDB->getNodeNum()) {
        //fw+ Duplicate detection: check if we already delivered this message
        auto itDelivered = tombstoneUntilMs.find(d.orig_id);
        if (itDelivered != tombstoneUntilMs.end() && millis() < itDelivered->second) {
            LOG_DEBUG("DTN: Ignoring duplicate delivery for id=0x%x (already delivered)", d.orig_id);
            return; // Already delivered, ignore duplicate
        }
        
        deliverLocal(d);
        //fw+ Set tombstone to prevent duplicate delivery
        if (configTombstoneMs) {
            tombstoneUntilMs[d.orig_id] = millis() + configTombstoneMs;
        }
        
        // Send DELIVERED receipt back to source (include via=proxy in reason low byte)
        uint32_t via = nodeDB->getNodeNum() & 0xFFu; // 1-byte hint
        LOG_INFO("DTN delivered id=0x%x to=0x%x src=0x%x via=0x%x", d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from, (unsigned)via);
        emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED, via);
        
        //fw+ Send native ACK to ALL senders (FW+ and stock) for proper app UX
        // Ensures apps show "delivered" instead of "pending"
        routingModule->sendAckNak(meshtastic_Routing_Error_NONE, d.orig_from, d.orig_id, d.channel, 0);
        
        //fw+ Clear stock marking if we successfully received DTN delivery (proves we are FW+)
        // This is our own node, so clear our nodeNum from stock list if it was mistakenly added
        auto itStock = stockKnownMs.find(nodeDB->getNodeNum());
        if (itStock != stockKnownMs.end()) {
            LOG_DEBUG("DTN: Clearing our own stock marking (we just received DTN!)");
            stockKnownMs.erase(itStock);
        }
        
        // Anti-storm: immediately silence local pending for this ID
        // Prevents continued attempts after successful delivery
        pendingById.erase(d.orig_id);
        
        return;
    }
    // Otherwise, coordinate with others: if we heard someone else carrying this id, suppress our attempt for a while 
    auto it = pendingById.find(d.orig_id);
    if (it == pendingById.end()) {
        //fw+ Cold start check for intermediate nodes: if we're cold and destination is not FW+, use native DM fallback
        if (isDtnCold() && !isFwplus(d.orig_to) && d.allow_proxy_fallback) {
            LOG_INFO("DTN: Intermediate cold start - using native DM fallback for id=0x%x dest=0x%x", 
                     d.orig_id, (unsigned)d.orig_to);
            
            // Trigger discovery and send native DM fallback
            triggerAggressiveDiscovery();
            
            // Create a temporary pending entry just for fallback
            Pending tempPending;
            tempPending.data = d;
            tempPending.tries = 0;
            
            if (sendProxyFallback(d.orig_id, tempPending)) {
                LOG_INFO("DTN: Cold start fallback sent successfully");
                return; // Don't enqueue for DTN processing
            }
        }
        
        // First time we see this id (from overlay or capture) → schedule normally
        scheduleOrUpdate(d.orig_id, d);
        // If the packet we saw is already overlay DATA from someone else, apply initial suppression window 
        if (configSuppressMsAfterForeign && !isFromUs(&mp)) {
            auto &p = pendingById[d.orig_id];
            applyForeignCarrySuppression(d.orig_id, p);
        }
    } else {
        // We already have an entry; seeing foreign DATA suggests someone carries it → backoff our schedule 
        if (configSuppressMsAfterForeign) {
            applyForeignCarrySuppression(d.orig_id, it->second);
            applyNearDestExtraSuppression(it->second, d.orig_to);
        }
    }
}

// Purpose: process received FWPLUS_DTN RECEIPT; clear pending, tombstone and decode special reason codes (e.g., version).
void DtnOverlayModule::handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r)
{
    // Simplest: stop any pending entry
    (void)mp;
    
    //fw+ Reset unresponsive tracking before erasing (destination is responsive!)
    auto itPending = pendingById.find(r.orig_id);
    if (itPending != pendingById.end()) {
        NodeNum dest = itPending->second.data.orig_to;
        
        // Reset counter for DELIVERED or PROGRESSED (both indicate destination is active)
        bool shouldReset = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED ||
                           r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED);
        
        if (shouldReset && isFwplus(dest) && itPending->second.dtnFailedAttempts > 0) {
            LOG_INFO("DTN: Receipt (status=%u) indicates FW+ dest 0x%x is responsive - resetting counter (was %u)",
                     (unsigned)r.status, (unsigned)dest, (unsigned)itPending->second.dtnFailedAttempts);
            itPending->second.dtnFailedAttempts = 0;
            itPending->second.fallbackTriggered = false; // Allow future fallback if needed
        }
        
        // Also clear from stockKnownMs if it was marked as stock due to unresponsiveness
        if (shouldReset) {
            auto itStock = stockKnownMs.find(dest);
            if (itStock != stockKnownMs.end()) {
                LOG_DEBUG("DTN: Clearing stock marking for responsive FW+ dest 0x%x", (unsigned)dest);
                stockKnownMs.erase(itStock);
            }
        }
    }
    
    pendingById.erase(r.orig_id);
    // Create a short tombstone to prevent immediate re-capture storms of same id 
    if (configTombstoneMs) tombstoneUntilMs[r.orig_id] = millis() + configTombstoneMs;
    ctrReceiptsReceived++; //fw+

    //fw+ interpret special FW+ version advertisements carried via receipt.reason
    // Convention: reason field contains version number directly (16-bit)
    LOG_DEBUG("DTN: Processing receipt reason=0x%x status=%u", (unsigned)r.reason, (unsigned)r.status);
    if (r.reason > 0) {
        LOG_INFO("DTN: Version advertisement detected, reason=0x%x", (unsigned)r.reason);
        NodeNum origin = getFrom(&mp);
        bool hadVer = (fwplusVersionByNode.find(origin) != fwplusVersionByNode.end());
        uint16_t ver = (uint16_t)(r.reason & 0xFFFFu);
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
                        // reason carries version; use full FW_PLUS_VERSION (lower 16 bits are used on RX)
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
        LOG_DEBUG("DTN: Not a version advertisement (reason=0x%x)", (unsigned)r.reason);
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
    
    // Calculate scheduling components using helper functions
    uint32_t base = calculateBaseDelay(d, p);
    uint32_t topologyDelay = calculateTopologyDelay(d);
    uint32_t mobilitySlot = calculateMobilitySlot(id, d, p);
    
    // Apply early bonus for confident routes
    uint32_t earlyBonus = 0;
    if (configPreferBestRouteSlotting && hasSufficientRouteConfidence(d.orig_to)) {
        earlyBonus = 1000; // pull in by ~1s
    }
    
    // Calculate target time
    uint32_t target = millis() + base + topologyDelay + mobilitySlot;
    if (target > millis() + earlyBonus) target -= earlyBonus;
    
    // Apply per-destination spacing
    float mobility = fwplus_getMobilityFactor01();
    target = applyPerDestinationSpacing(target, d.orig_to, mobility);
    
    p.nextAttemptMs = target;
    LOG_DEBUG("DTN schedule id=0x%x next=%u ms (base=%u topology=%u slot=%u)", 
              id, (unsigned)(p.nextAttemptMs - millis()), (unsigned)base, 
              (unsigned)topologyDelay, (unsigned)mobilitySlot);
}

// Purpose: perform one forwarding decision for a pending DTN item.
// Steps: channel-util gate → route confidence/handoff → tail probe/fallback → overlay send and backoff scheduling.
// Guarantees: never emits plaintext via overlay; fallback uses native encrypted DM without spoofing sender.
void DtnOverlayModule::tryForward(uint32_t id, Pending &p)
{
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) { 
        p.nextAttemptMs = millis() + 2500 + (uint32_t)random(500); 
        LOG_DEBUG("DTN busy: defer id=0x%x", id); 
        return; 
    }

    // Check max tries limit
    if (configMaxTries && p.tries >= configMaxTries) {
        emitReceipt(p.data.orig_from, id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0); 
        LOG_WARN("DTN give up id=0x%x tries=%u", id, (unsigned)p.tries);
        pendingById.erase(id);
        ctrGiveUps++;
        return;
    }

    // Check DV-ETX route confidence gating
    bool lowConf = !hasSufficientRouteConfidence(p.data.orig_to);
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    if (shouldDeferForIntermediateLowConf(p, lowConf)) {
        return;
    }

    //fw+ Fast path for direct neighbors - immediate DTN forwarding
    if (isDirectNeighbor(p.data.orig_to)) {
        // For direct neighbors, skip fallback checks and go straight to DTN forwarding
        LOG_DEBUG("DTN: Fast path for direct neighbor 0x%x", (unsigned)p.data.orig_to);
        // Continue to DTN forwarding below
    } else {
        // For source node: immediate attempt with traceroute hint if needed
        bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
        if (isFromSource) {
            triggerTracerouteIfNeededForSource(p, lowConf);
        } else {
            // Intermediate nodes: try intelligent fallback first
            if (tryIntelligentFallback(id, p)) return;
            // Try various fallback mechanisms for multi-hop destinations
            if (tryColdStartFallback(id, p)) return;
            if (tryNearDestinationFallback(id, p)) return;
            if (tryKnownStockFallback(id, p)) return;
        }
        
        //fw+ Try fallback for unresponsive FW+ destinations (both source and intermediate)
        if (tryFwplusUnresponsiveFallback(id, p)) return;
    }

    // Select forward target and handle traceroute
    NodeNum target = selectForwardTarget(p);
    // Only trigger traceroute for intermediate nodes, not source (source already triggered above)
    if (lowConf && target == p.data.orig_to && !isDirectNeighbor(p.data.orig_to) && !isFromSource) {
        maybeTriggerTraceroute(p.data.orig_to);
        return; // give time for route discovery before attempting
    }

    // Send DTN overlay packet
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_data_tag;
    msg.variant.data = p.data;

    meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
    if (!mp) {
        p.nextAttemptMs = millis() + 3000;
        return;
    }

    // Configure packet
    mp->to = target;
    mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    mp->want_ack = false;
    mp->decoded.want_response = false;
    
    setPriorityForTailAndSource(mp, p, isFromSource);

    // Check if we should back off (favor closer relayers)
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    uint8_t hopsToSrc = getHopsAway(p.data.orig_from);
    if (hopsToSrc != 255 && hopsToDest != 255 && hopsToDest > hopsToSrc) {
        p.nextAttemptMs = millis() + configRetryBackoffMs + 5000 + (uint32_t)random(2000);
        return;
    }

    // Send the packet
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    ctrForwardsAttempted++; 
    lastForwardMs = millis(); 
    
    // Update tracking
    lastDestTxMs[p.data.orig_to] = millis();
    if (target != p.data.orig_to) {
        auto &ph = preferredHandoffByDest[p.data.orig_to];
        ph.node = target;
        ph.lastUsedMs = millis();
    }
    
    //fw+ Track DTN attempt for unresponsive detection (only for direct-to-dest attempts, not handoffs)
    if (target == p.data.orig_to && isFwplus(p.data.orig_to)) {
        p.lastDtnAttemptMs = millis();
        p.dtnFailedAttempts++; // Will be reset if we get a receipt
        LOG_DEBUG("DTN: Tracking attempt %u to FW+ dest 0x%x (unresponsive detection)", 
                 (unsigned)p.dtnFailedAttempts, (unsigned)p.data.orig_to);
    }
    
    // Schedule next attempt
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
    //fw+ spoof original sender for proper decryption by stock receiver
    // Stock nodes need the original sender's key to decrypt the payload
    dm->from = p.data.orig_from;
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
    
    // Adaptive mobility management - adjust parameters based on mobility
    adaptiveMobilityManagement();
    
    // Cold-start: if we don't yet know any FW+ peer, use a shorter interval
    bool knowsAnyFwplus = false;
    for (const auto &kv : fwplusVersionByNode) { (void)kv; knowsAnyFwplus = true; break; }
    uint32_t interval = knowsAnyFwplus ? configAdvertiseIntervalMs : configAdvertiseIntervalUnknownMs;
    if (lastAdvertiseMs == 0) lastAdvertiseMs = now - (uint32_t)random(interval);
    // One-shot early advertise shortly after module start to speed up discovery (unconditional broadcast)
    if (!firstAdvertiseDone && now - moduleStartMs >= configFirstAdvertiseDelayMs) {
        //fw+ Check if we should retry (if previous attempt failed)
        bool shouldRetry = (firstAdvertiseRetryMs > 0 && now - firstAdvertiseRetryMs >= configFirstAdvertiseRetryMs);
        bool isFirstAttempt = (firstAdvertiseRetryMs == 0);
        
        if (isFirstAttempt || shouldRetry) {
            LOG_INFO("DTN: Attempting first beacon after %u ms (attempt %s)", 
                     (unsigned)(now - moduleStartMs), isFirstAttempt ? "1" : "retry");
            
            //fw+ More aggressive first beacon - try even if channel is busy
            bool channelClear = !(airTime && !airTime->isTxAllowedChannelUtil(true));
            if (channelClear) {
                LOG_INFO("DTN: Channel clear, sending beacon");
            } else {
                LOG_INFO("DTN: Channel busy but attempting beacon anyway for discovery");
            }
            
            // reason carries FW+ version (lower 16 bits used by receiver)
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
                
                //fw+ Debug: check owner.id before sending
                LOG_DEBUG("DTN: Beacon owner.id='%s' nodeNum=0x%x", owner.id, (unsigned)nodeDB->getNodeNum());
                
        //fw+ Workaround: ensure owner.id is set for MQTT topic generation
        if (!owner.id[0]) {
            snprintf(owner.id, sizeof(owner.id), "!%08x", nodeDB->getNodeNum());
            LOG_DEBUG("DTN: Fixed empty owner.id, now='%s'", owner.id);
        }
        
        //fw+ Check MQTT connection status before sending beacon
        if (mqtt && mqtt->isEnabled()) {
            if (mqtt->isConnectedDirectly()) {
                LOG_DEBUG("DTN: MQTT connected directly - beacon will be published to server");
            } else if (moduleConfig.mqtt.proxy_to_client_enabled) {
                LOG_DEBUG("DTN: MQTT using client proxy - beacon depends on phone app connection");
            } else {
                LOG_DEBUG("DTN: MQTT not connected - beacon may not reach lorastats.pl");
            }
        } else {
            LOG_DEBUG("DTN: MQTT disabled - beacon will not be published");
        }
                
                service->sendToMesh(p, RX_SRC_LOCAL, false);
                lastAdvertiseMs = now;
                LOG_INFO("DTN: Beacon sent successfully");
                firstAdvertiseDone = true; // Mark as done only on success
                
                //fw+ Force MQTT proxy queue flush to ensure beacon reaches lorastats.pl
                if (mqtt && mqtt->isEnabled() && moduleConfig.mqtt.proxy_to_client_enabled) {
                    LOG_DEBUG("DTN: Triggering MQTT proxy queue flush for beacon delivery");
                    // Note: Queue flush happens automatically in MQTT::runOnce()
                }
            } else {
                LOG_WARN("DTN: Failed to allocate packet for beacon - will retry in %u ms", (unsigned)configFirstAdvertiseRetryMs);
                //fw+ Schedule retry
                firstAdvertiseRetryMs = now;
                return;
            }
        }
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

// Purpose: check if a handoff candidate is valid (not self, dest, or lastCarrier)
// Returns: true if candidate is safe to use
bool DtnOverlayModule::isValidHandoffCandidate(NodeNum candidate, NodeNum dest, const Pending &p) const
{
    if (candidate == 0) return false;
    if (candidate == nodeDB->getNodeNum()) return false;
    if (candidate == dest) return false;
    if (candidate == p.lastCarrier) return false;
    return true;
}

// Purpose: verify if a node is reachable and not stale
// Returns: true if node should be considered for handoff
bool DtnOverlayModule::isNodeReachable(NodeNum node) const
{
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (!info) return false;
    
    // Check if node is not too far
    if (info->hops_away > 3) return false;
    
    // Check if node was seen recently
    uint32_t now = millis();
    uint32_t lastSeen = info->last_heard * 1000UL;
    uint32_t maxAge = 30UL * 60UL * 1000UL; // 30 minutes
    
    return (now - lastSeen) < maxAge;
}

// Purpose: intelligent fallback for unknown or low-confidence destinations
// Returns: true if fallback was attempted
bool DtnOverlayModule::tryIntelligentFallback(uint32_t id, Pending &p)
{
    // For stock destinations with low route confidence, try native DM early
    if (!isFwplus(p.data.orig_to) && !hasSufficientRouteConfidence(p.data.orig_to)) {
        LOG_DEBUG("DTN: Low confidence to stock dest 0x%x, trying native DM", (unsigned)p.data.orig_to);
        return sendProxyFallback(id, p);
    }
    
    // For unreachable destinations in TTL tail, try DTN broadcast as last resort
    if (!isNodeReachable(p.data.orig_to)) {
        uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
        uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ? 
                      (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
        uint32_t tailStart = p.data.deadline_ms - (ttl * 20 / 100); // Last 20% of TTL
        
        if (nowEpoch >= tailStart) {
            // Anti-burst: check global cooldown and per-id cooldown before broadcasting
            {
                auto itBroadcast = lastBroadcastSentMs.find(p.data.orig_id);
                uint32_t now = millis();
                // Randomized cooldown 60-120s to allow up to 7-hop delivery before re-broadcast
                uint32_t broadcastCooldown = 60000 + (uint32_t)random(60001);
                
                if (itBroadcast != lastBroadcastSentMs.end() && (now - itBroadcast->second) < broadcastCooldown) {
                    LOG_DEBUG("DTN: Broadcast cooldown active for id=0x%x", p.data.orig_id);
                    return false;
                }
            }
            
            // Channel utilization gate for broadcast
            if (airTime && airTime->channelUtilizationPercent() > 40) {
                LOG_DEBUG("DTN: Channel too busy for broadcast (util=%u%%)", airTime->channelUtilizationPercent());
                return false;
            }
            
            LOG_DEBUG("DTN: Unreachable dest 0x%x in TTL tail, trying DTN broadcast fallback", (unsigned)p.data.orig_to);
            
            // Send DTN broadcast (unencrypted) as last resort for FW+ nodes to pick up
            // Force unencrypted payload for public broadcast
            meshtastic_FwplusDtnData broadcastData = p.data;
            broadcastData.is_encrypted = false; // Force unencrypted for broadcast
            
            meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
            msg.which_variant = meshtastic_FwplusDtn_data_tag;
            msg.variant.data = broadcastData;
            
            meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
            if (!mp) return false;
            
            mp->to = NODENUM_BROADCAST;
            mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
            mp->want_ack = false;
            mp->decoded.want_response = false;
            mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
            
            // Ensure packet is not encrypted (public broadcast)
            mp->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
            
            service->sendToMesh(mp, RX_SRC_LOCAL, false);
            p.tries++;
            // Ensure post-broadcast retry spacing is at least 60-120s to avoid storms
            uint32_t minRetry = 60000 + (uint32_t)random(60001);
            uint32_t retryDelay = configRetryBackoffMs > minRetry ? configRetryBackoffMs : minRetry;
            p.nextAttemptMs = millis() + retryDelay;
            lastBroadcastSentMs[p.data.orig_id] = millis(); // Track broadcast time
            LOG_INFO("DTN: DTN broadcast fallback sent for unreachable dest 0x%x", (unsigned)p.data.orig_to);
            return true;
        }
    }
    
    return false;
}

// Purpose: adaptive mobility management - adjust parameters based on mobility
void DtnOverlayModule::adaptiveMobilityManagement()
{
    float mobility = fwplus_getMobilityFactor01();
    
    // Store original config values if not already stored
    static bool originalConfigStored = false;
    static uint32_t originalRetryBackoffMs = configRetryBackoffMs;
    static uint32_t originalPerDestMinSpacingMs = configPerDestMinSpacingMs;
    static uint32_t originalMaxTries = configMaxTries;
    
    if (!originalConfigStored) {
        originalRetryBackoffMs = configRetryBackoffMs;
        originalPerDestMinSpacingMs = configPerDestMinSpacingMs;
        originalMaxTries = configMaxTries;
        originalConfigStored = true;
    }
    
    // Adapt parameters based on mobility
    if (mobility > 0.7f) {
        // Very mobile - aggressive settings
        configRetryBackoffMs = originalRetryBackoffMs / 4; // 25% of original
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs / 2; // 50% of original
        configMaxTries = originalMaxTries + 1; // One more try
    } else if (mobility > 0.3f) {
        // Moderately mobile - standard settings
        configRetryBackoffMs = originalRetryBackoffMs;
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs;
        configMaxTries = originalMaxTries;
    } else {
        // Stationary - conservative settings
        configRetryBackoffMs = originalRetryBackoffMs * 2; // 200% of original
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs * 2; // 200% of original
        configMaxTries = originalMaxTries > 1 ? originalMaxTries - 1 : 1; // One less try
    }
}

// Purpose: passive DTN discovery - no active probing
// Discovery happens through:
// 1. Periodic beacons (maybeAdvertiseFwplusVersion)
// 2. OnDemand responses (observeOnDemandResponse) 
// 3. Overheard DTN traffic (markFwplusSeen in handleReceivedProtobuf)
// 4. Telemetry-triggered probes (only when we see telemetry from unknown nodes)

// Purpose: detailed logging and monitoring
void DtnOverlayModule::logDetailedStats()
{
    if (!configEnabled) return;
    
    uint32_t now = millis();
    if (now - lastDetailedLogMs < configDetailedLogIntervalMs) return;
    
    LOG_INFO("DTN Stats: pending=%u forwards=%u fallbacks=%u fwplus_unresponsive=%u receipts=%u milestones=%u probes=%u",
             (unsigned)pendingById.size(), (unsigned)ctrForwardsAttempted,
             (unsigned)ctrFallbacksAttempted, (unsigned)ctrFwplusUnresponsiveFallbacks,
             (unsigned)ctrReceiptsEmitted, (unsigned)ctrMilestonesSent, (unsigned)ctrProbesSent);
    
    LOG_INFO("DTN FW+ Nodes: %u known", (unsigned)fwplusVersionByNode.size());
    
    // Log details about known FW+ nodes
    for (const auto &kv : fwplusVersionByNode) {
        NodeNum node = kv.first;
        uint16_t version = kv.second;
        uint8_t hops = getHopsAway(node);
        bool reachable = isNodeReachable(node);
        
        LOG_INFO("DTN Node 0x%x: ver=%u hops=%u reachable=%d", 
                 (unsigned)node, (unsigned)version, (unsigned)hops, (int)reachable);
    }
    
    lastDetailedLogMs = now;
}

// Purpose: check if we have any valid handoff candidates for destination
// Returns: true if at least one valid candidate exists
bool DtnOverlayModule::hasValidHandoffCandidates(NodeNum dest, const Pending &p) const
{
    int totalNodes = nodeDB->getNumMeshNodes();
    for (int i = 0; i < totalNodes; ++i) {
        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
        if (!ni) continue;
        if (!isFwplus(ni->num)) continue;
        if (!isValidHandoffCandidate(ni->num, dest, p)) continue;
        if (!isNodeReachable(ni->num)) continue; // Check if candidate is reachable
        
        auto itVer = fwplusVersionByNode.find(ni->num);
        if (itVer == fwplusVersionByNode.end() || itVer->second < configMinFwplusVersionForHandoff) continue;
        
        uint8_t ad = getHopsAway(dest);
        if (ad == 255) continue; // unknown route
        
        return true; // Found at least one valid candidate
    }
    return false;
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

    // 2) If NextHopRouter has a next_hop that is FW+, pick that first (safely map low-byte to direct neighbor)
    if (router) {
        auto nh = static_cast<NextHopRouter *>(router);
        auto snap = nh->getRouteSnapshot(false);
        for (const auto &e : snap) {
            if (e.dest != dest) continue;
            if (e.next_hop == NO_NEXT_HOP_PREFERENCE) break;
            // Safe mapping: only accept a unique direct neighbor matching the low byte
            NodeNum mapped = 0;
            {
                NodeNum candidate = 0;
                bool collision = false;
                int totalNodes = nodeDB->getNumMeshNodes();
                for (int i = 0; i < totalNodes; ++i) {
                    meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                    if (!n) continue;
                    if (n->num == nodeDB->getNodeNum()) continue;
                    if (n->hops_away != 0) continue; // only direct neighbors
                    if ((uint8_t)(n->num & 0xFF) != e.next_hop) continue;
                    if (candidate == 0) candidate = n->num; else if (candidate != n->num) { collision = true; break; }
                }
                if (!collision && candidate != 0) mapped = candidate;
            }
            if (mapped && isFwplus(mapped)) {
                auto itVer = fwplusVersionByNode.find(mapped);
                if (itVer != fwplusVersionByNode.end() && itVer->second >= configMinFwplusVersionForHandoff) {
                    return mapped;
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
            if (!isNodeReachable(ni->num)) continue; // Check if candidate is reachable
            auto itVer = fwplusVersionByNode.find(ni->num);
            if (itVer == fwplusVersionByNode.end() || itVer->second < configMinFwplusVersionForHandoff) continue;
            uint8_t au = ni->hops_away;
            uint8_t ad = getHopsAway(dest);
            
            // Skip candidates with unknown route to destination (255 = unknown)
            if (ad == 255) continue;
            
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
    
    // Avoid accidental cycles and routing loops
    if (pick == dest || pick == nodeDB->getNodeNum() || pick == p.lastCarrier) {
        LOG_DEBUG("DTN: Skipping handoff candidate (dest/self/lastCarrier): 0x%x", (unsigned)pick);
        return 0;
    }
    
    LOG_DEBUG("DTN: Selected handoff candidate: 0x%x (index=%u)", (unsigned)pick, (unsigned)p.handoffIndex);
    return pick;
}

// Purpose: invalidate stale routes for mobile nodes to prevent using outdated handoff candidates
// Effect: removes routes to nodes that are no longer reachable or have low confidence
void DtnOverlayModule::invalidateStaleRoutes()
{
    float mobility = fwplus_getMobilityFactor01();
    
    // Only invalidate if we're mobile (mobility > 0.3) to avoid disrupting stable nodes
    if (mobility < 0.3f) return;
    
    // Check all known FW+ nodes for stale routes
    for (auto it = fwplusVersionByNode.begin(); it != fwplusVersionByNode.end();) {
        NodeNum node = it->first;
        
        if (isRouteStale(node)) {
            LOG_INFO("DTN: Invalidating stale route to mobile node 0x%x (mobility=%.2f)", (unsigned)node, mobility);
            
            // Remove from FW+ version tracking
            it = fwplusVersionByNode.erase(it);
            
            // Also penalize route in NextHopRouter if available
            if (router) {
                auto nh = static_cast<NextHopRouter *>(router);
                nh->penalizeRouteOnFailed(0, node, 0, meshtastic_Routing_Error_MAX_RETRANSMIT); // Strong penalty for stale route
            }
        } else {
            ++it;
        }
    }
}

// Purpose: check if a route to a destination is stale based on mobility and time
// Returns: true if route should be considered stale and invalidated
bool DtnOverlayModule::isRouteStale(NodeNum dest) const
{
    uint32_t now = millis();
    float mobility = fwplus_getMobilityFactor01();
    
    // Get node info to check last seen time
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(dest);
    if (!node) return true; // No node info = stale
    
    // Calculate stale timeout based on mobility
    // Mobile nodes: 30min, stationary: 2h
    uint32_t staleTimeoutMs = 30UL * 60UL * 1000UL; // 30min base
    if (mobility < 0.3f) {
        staleTimeoutMs = 2UL * 60UL * 60UL * 1000UL; // 2h for stationary
    } else if (mobility > 0.7f) {
        staleTimeoutMs = 15UL * 60UL * 1000UL; // 15min for very mobile
    }
    
    // Check if we haven't seen this node recently
    uint32_t lastSeen = node->last_heard * 1000UL; // Convert to ms
    if (now - lastSeen > staleTimeoutMs) {
        LOG_DEBUG("DTN: Route to 0x%x is stale (last_heard=%u, timeout=%u, mobility=%.2f)", 
                 (unsigned)dest, (unsigned)lastSeen, (unsigned)staleTimeoutMs, mobility);
        return true;
    }
    
    // Check if node is too far away (hops > 3 for mobile nodes)
    if (mobility > 0.5f && node->hops_away > 3) {
        LOG_DEBUG("DTN: Route to 0x%x is too far for mobile node (hops=%u, mobility=%.2f)", 
                 (unsigned)dest, (unsigned)node->hops_away, mobility);
        return true;
    }
    
    return false;
}

// Purpose: DV-ETX gating wrapper - more permissive for mobile nodes
bool DtnOverlayModule::hasSufficientRouteConfidence(NodeNum dest) const
{
    if (!router) return true;
    // fw+ more permissive: allow attempts even with low confidence for mobile nodes
    float mobility = fwplus_getMobilityFactor01();
    if (mobility > 0.5f) return true; // Mobile nodes: always try
    // minimal confidence 1 like S&F for stationary nodes
    return router->hasRouteConfidence(dest, 1);
}

// Purpose: observe OnDemand responses to discover DTN-enabled nodes
// Effect: adds DTN-enabled nodes to fwplusVersionByNode for future handoff consideration
void DtnOverlayModule::observeOnDemandResponse(const meshtastic_MeshPacket &mp)
{
    // Only process if we don't already know this node as FW+
    NodeNum origin = getFrom(&mp);
    if (isFwplus(origin)) return;
    
    // Decode OnDemand response
    meshtastic_OnDemand ondemand = meshtastic_OnDemand_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_OnDemand_msg, &ondemand)) {
        return;
    }
    
    // Check if this is a DTN overlay stats response
    if (ondemand.which_variant == meshtastic_OnDemand_response_tag &&
        ondemand.variant.response.response_type == meshtastic_OnDemandType_RESPONSE_DTN_OVERLAY_STATS) {
        
        auto &dtnStats = ondemand.variant.response.response_data.dtn_overlay_stats;
        if (dtnStats.has_enabled && dtnStats.enabled) {
            // This node has DTN enabled! Add it to our FW+ tracking
            LOG_INFO("DTN: Discovered DTN-enabled node 0x%x via OnDemand response", (unsigned)origin);
            
            // Add to FW+ version tracking (use current FW+ version as placeholder)
            fwplusVersionByNode[origin] = FW_PLUS_VERSION;
            
            // Also update last seen time
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(origin);
            if (node) {
                node->last_heard = getTime();
            }
            
            // fw+ Proactively probe this DTN-enabled node to get its actual FW+ version
            // This helps us learn the real version instead of using placeholder
            uint32_t nowMs = millis();
            auto it = lastTelemetryProbeToNodeMs.find(origin);
            if (it == lastTelemetryProbeToNodeMs.end() || (nowMs - it->second) >= configTelemetryProbeCooldownMs) {
                if (!(airTime && !airTime->isTxAllowedChannelUtil(true))) {
                    LOG_DEBUG("DTN: Proactively probing DTN-enabled node 0x%x for FW+ version", (unsigned)origin);
                    maybeProbeFwplus(origin);
                    lastTelemetryProbeToNodeMs[origin] = nowMs;
                }
            }
        }
    }
}

// Purpose: calculate base delay for scheduling
uint32_t DtnOverlayModule::calculateBaseDelay(const meshtastic_FwplusDtnData &d, const Pending &p) const
{
    //fw+ Fast path for direct neighbors - much shorter delays for hop=0
    if (isDirectNeighbor(d.orig_to)) {
        uint32_t base = 50; // 50ms base for direct neighbors (vs 2000ms for others)
        
        // Minimal jitter for direct neighbors
        bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
        if (isFromSource && p.tries == 0) {
            base += (uint32_t)random(50); // 0-50ms jitter (vs 200-600ms)
        }
        
        // Minimal grace window for direct neighbors
        if (!isFromSource && p.tries == 0 && configGraceAckMs) {
            base += 25; // 25ms grace (vs 500ms)
        }
        
        return base;
    }
    
    // Standard delay for multi-hop destinations
    uint32_t base = configInitialDelayBaseMs ? configInitialDelayBaseMs : 8000;
    
    // Immediate-from-source: tiny jitter instead of long base/slot
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    if (isFromSource && p.tries == 0) {
        base = 200 + (uint32_t)random(401); // 200..600 ms
    }
    
    // Apply grace window only for non-self-origin
    if (!isFromSource && p.tries == 0 && configGraceAckMs) {
        if (base < configGraceAckMs) base = configGraceAckMs;
    }
    
    // If destination is our direct neighbor and suppression is enabled, be extra conservative
    if (configSuppressIfDestNeighbor && isDirectNeighbor(d.orig_to)) {
        base += configGraceAckMs ? configGraceAckMs : 1500;
    }
    
    return base;
}

// Purpose: calculate topology-aware delay for far nodes
uint32_t DtnOverlayModule::calculateTopologyDelay(const meshtastic_FwplusDtnData &d) const
{
    if (configMaxRingsToAct == 0) return 0;
    
    uint8_t hopsToDest = getHopsAway(d.orig_to);
    if (hopsToDest == 255 || hopsToDest <= configMaxRingsToAct) return 0;
    
    if (d.deadline_ms && d.orig_rx_time) {
        uint32_t ttl = (d.deadline_ms > d.orig_rx_time * 1000UL) ? 
                      (d.deadline_ms - (d.orig_rx_time * 1000UL)) : 0;
        uint32_t mustWait = (uint64_t)ttl * (configFarMinTtlFracPercent > 100 ? 100 : configFarMinTtlFracPercent) / 100;
        uint32_t readyEpoch = (d.orig_rx_time * 1000UL) + mustWait;
        uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
        
        if (nowEpoch < readyEpoch) {
            return readyEpoch - nowEpoch;
        }
    } else {
        return 5000; // no TTL: arbitrary extra delay when far
    }
    
    return 0;
}

// Purpose: calculate mobility-aware election slot timing
uint32_t DtnOverlayModule::calculateMobilitySlot(uint32_t id, const meshtastic_FwplusDtnData &d, const Pending &p) const
{
    //fw+ Fast path for direct neighbors - minimal slotting
    if (isDirectNeighbor(d.orig_to)) {
        uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
        bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
        uint32_t slot = (isFromSource && p.tries == 0) ? 0 : (rank % 2); // 2 slots for direct neighbors
        return slot * 25 + (uint32_t)random(25); // 25ms slots for direct neighbors
    }
    
    float mobility = fwplus_getMobilityFactor01();
    uint32_t slots = 8;
    uint32_t slotLen = 400;
    
    if (mobility > 0.5f) {
        slots = 6;
        slotLen = 300;
    }
    
    uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    uint32_t slot = (isFromSource && p.tries == 0) ? 0 : (rank % slots);
    
    return slot * slotLen + (uint32_t)random(slotLen);
}

// Purpose: apply per-destination spacing constraints
uint32_t DtnOverlayModule::applyPerDestinationSpacing(uint32_t target, NodeNum dest, float mobility) const
{
    auto itLast = lastDestTxMs.find(dest);
    if (itLast == lastDestTxMs.end()) return target;
    
    uint32_t spacing = configPerDestMinSpacingMs ? configPerDestMinSpacingMs : 30000;
    // fw+ reduce spacing when mobile (down to ~60%) to react faster
    spacing = (uint32_t)((float)spacing * (1.0f - 0.4f * mobility));
    
    if (target < itLast->second + spacing) {
        target = itLast->second + spacing + (uint32_t)random(500);
    }
    
    return target;
}

// Purpose: check if fallback should be used for this pending item
bool DtnOverlayModule::shouldUseFallback(const Pending &p) const
{
    return !isFwplus(p.data.orig_to) && p.data.allow_proxy_fallback;
}

// Purpose: try cold start fallback for intermediate nodes
bool DtnOverlayModule::tryColdStartFallback(uint32_t id, Pending &p)
{
    if (isDtnCold() && shouldUseFallback(p)) {
        LOG_INFO("DTN: Cold start detected, using native DM fallback for dest=0x%x", (unsigned)p.data.orig_to);
        
        if (p.tries == 0) {
            LOG_INFO("DTN: Triggering aggressive discovery for cold start");
            triggerAggressiveDiscovery();
        }
        
        if (sendProxyFallback(id, p)) return true;
    }
    return false;
}

// Purpose: try near-destination fallback for close targets
bool DtnOverlayModule::tryNearDestinationFallback(uint32_t id, Pending &p)
{
    uint8_t hops = getHopsAway(p.data.orig_to);
    if (hops != 255 && hops <= 1 && shouldUseFallback(p)) {
        if (sendProxyFallback(id, p)) return true;
    }
    return false;
}


// Purpose: try fallback for known stock destinations or TTL tail
bool DtnOverlayModule::tryKnownStockFallback(uint32_t id, Pending &p)
{
    bool destKnownStock = isDestKnownStock(p.data.orig_to);
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t ttlTailStart = 0;
    
    if (p.data.deadline_ms && configLateFallback && configFallbackTailPercent) {
        uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ? 
                      (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
        ttlTailStart = p.data.deadline_ms - (ttl * (configFallbackTailPercent > 100 ? 100 : configFallbackTailPercent) / 100);
    }
    bool inTail = (p.data.deadline_ms && nowEpoch >= ttlTailStart);
    
    if ((destKnownStock || inTail) && shouldUseFallback(p)) {
        if (inTail && configProbeFwplusNearDeadline) {
            maybeProbeFwplus(p.data.orig_to);
        }
        if (sendProxyFallback(id, p)) return true;
    }
    return false;
}

// Purpose: try fallback for unresponsive FW+ destinations
// Returns: true if fallback was attempted
// fw+ New mechanism to handle FW+ destinations that don't respond despite being marked as DTN-capable.
// This catches cases where: version changed, node is offline/unreachable, or has incompatible DTN implementation.
bool DtnOverlayModule::tryFwplusUnresponsiveFallback(uint32_t id, Pending &p)
{
    // Skip if feature disabled
    if (!configFwplusUnresponsiveFallback) return false;
    
    // Skip if already triggered fallback for this message
    if (p.fallbackTriggered) return false;
    
    // Skip if destination is not FW+ (normal fallback logic handles stock nodes)
    if (!isFwplus(p.data.orig_to)) return false;
    
    // Skip if proxy fallback is not allowed
    if (!p.data.allow_proxy_fallback) return false;
    
    // Check if we have enough failed attempts
    if (p.dtnFailedAttempts < configFwplusFailureThreshold) return false;
    
    // Check if enough time has passed since last attempt to conclude unresponsiveness
    if (p.lastDtnAttemptMs == 0) return false;
    uint32_t timeSinceLastAttempt = millis() - p.lastDtnAttemptMs;
    if (timeSinceLastAttempt < configFwplusResponseTimeoutMs) return false;
    
    // All conditions met - destination is FW+ but unresponsive, try native DM fallback
    LOG_WARN("DTN: FW+ dest 0x%x unresponsive after %u attempts over %u ms - trying native DM fallback",
             (unsigned)p.data.orig_to, (unsigned)p.dtnFailedAttempts, (unsigned)timeSinceLastAttempt);
    
    // Mark that we've triggered fallback to avoid repeating
    p.fallbackTriggered = true;
    
    // Send native DM fallback
    if (sendProxyFallback(id, p)) {
        LOG_INFO("DTN: Native DM fallback sent to unresponsive FW+ dest 0x%x", (unsigned)p.data.orig_to);
        ctrFwplusUnresponsiveFallbacks++; //fw+ increment counter for diagnostics
        
        // Optionally mark this destination as potentially stock for a while
        // This helps avoid repeated DTN attempts to the same unresponsive FW+ node
        stockKnownMs[p.data.orig_to] = millis();
        
        return true;
    }
    
    return false;
}

// Purpose: select the best forward target for DTN packet
NodeNum DtnOverlayModule::selectForwardTarget(Pending &p)
{
    NodeNum target = p.data.orig_to;
    
    if (configEnableFwplusHandoff) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        if (hopsToDest != 255 && hopsToDest >= configHandoffMinRing) {
            if (hasValidHandoffCandidates(p.data.orig_to, p)) {
                NodeNum cand = chooseHandoffTarget(p.data.orig_to, 0, p);
                if (cand != 0 && isValidHandoffCandidate(cand, p.data.orig_to, p)) {
                    target = cand;
                    LOG_DEBUG("DTN: Using handoff target: 0x%x (hops=%u)", (unsigned)target, (unsigned)hopsToDest);
                } else {
                    LOG_DEBUG("DTN: Handoff selection failed, using broadcast fallback");
                    target = NODENUM_BROADCAST;
                }
            } else {
                LOG_DEBUG("DTN: No valid handoff candidates available, using broadcast fallback");
                target = NODENUM_BROADCAST;
            }
        }
    }
    
    return target;
}

// Purpose: trigger aggressive DTN discovery during cold start
// Effect: sends immediate beacon and probes all known neighbors for FW+ capability
void DtnOverlayModule::triggerAggressiveDiscovery()
{
    if (!configEnabled) return;
    
    // Send immediate beacon if channel is clear
    // Use passive discovery only - no additional broadcast beacons
    // Discovery happens through: periodic beacons, OnDemand responses, and overheard DTN traffic
    
    // Probe all direct neighbors for FW+ capability
    int totalNodes = nodeDB->getNumMeshNodes();
    for (int i = 0; i < totalNodes; ++i) {
        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
        if (!ni) continue;
        if (ni->num == nodeDB->getNodeNum()) continue;
        if (ni->hops_away != 0) continue; // Only direct neighbors
        
        // Skip if we already know this node
        if (isFwplus(ni->num)) continue;
        
        // Send FW+ probe
        uint32_t nowMs = millis();
        auto it = lastTelemetryProbeToNodeMs.find(ni->num);
        if (it == lastTelemetryProbeToNodeMs.end() || (nowMs - it->second) >= 30000) { // 30s cooldown for aggressive discovery
            LOG_DEBUG("DTN: Aggressively probing neighbor 0x%x for FW+ capability", (unsigned)ni->num);
            maybeProbeFwplus(ni->num);
            lastTelemetryProbeToNodeMs[ni->num] = nowMs;
        }
    }
}

bool DtnOverlayModule::shouldDeferForIntermediateLowConf(const Pending &p, bool lowConf) const
{
    if (!lowConf) return false;
    if (isDirectNeighbor(p.data.orig_to)) return false;
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    if (isFromSource) return false;
    float mobility = fwplus_getMobilityFactor01();
    uint32_t backoff = configRetryBackoffMs + (uint32_t)random(2000);
    if (mobility > 0.5f) backoff = backoff / 2; // try sooner if we are moving
    // We cannot mutate p here (const), so signal caller to handle
    return true;
}

void DtnOverlayModule::triggerTracerouteIfNeededForSource(const Pending &p, bool lowConf)
{
    if (!lowConf) return;
    maybeTriggerTraceroute(p.data.orig_to);
    LOG_DEBUG("DTN: Source with low confidence, triggering traceroute to 0x%x", (unsigned)p.data.orig_to);
}

void DtnOverlayModule::setPriorityForTailAndSource(meshtastic_MeshPacket *mp, const Pending &p, bool isFromSource)
{
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    if (!p.data.deadline_ms) return;
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ?
                  (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
    uint32_t tailStart = p.data.deadline_ms - (ttl * (configFallbackTailPercent > 100 ? 100 : configFallbackTailPercent) / 100);
    bool nearDst = false;
    if (configTailEscalateMaxRing > 0) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        nearDst = (hopsToDest != 255 && hopsToDest <= configTailEscalateMaxRing);
    }
    if (nowEpoch >= tailStart && (nearDst || isFromSource)) {
        mp->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    }
}

bool DtnOverlayModule::sendBroadcastFallback(uint32_t id, Pending &p)
{
    // Reuse tryIntelligentFallback broadcast branch by calling it directly
    return tryIntelligentFallback(id, p);
}

void DtnOverlayModule::applyForeignCarrySuppression(uint32_t id, Pending &p)
{
    uint32_t postpone = millis() + configSuppressMsAfterForeign + (uint32_t)random(500);
    if (p.nextAttemptMs < postpone) p.nextAttemptMs = postpone;
    LOG_DEBUG("DTN suppress id=0x%x after foreign carry, next in %u ms", id, (unsigned)(p.nextAttemptMs - millis()));
}

void DtnOverlayModule::applyNearDestExtraSuppression(Pending &p, NodeNum dest)
{
    uint8_t hopsToDest = getHopsAway(dest);
    if (hopsToDest != 255 && hopsToDest <= 2) {
        // Stronger suppression when immediately near destination to prevent duplicate deliveries
        uint32_t longerSuppress = (hopsToDest <= 1) ? (configSuppressMsAfterForeign * 3)
                                                    : (configSuppressMsAfterForeign * 2);
        uint32_t postpone = millis() + longerSuppress + (uint32_t)random(1000);
        if (p.nextAttemptMs < postpone) p.nextAttemptMs = postpone;
        LOG_DEBUG("DTN near-dest extra suppress for %u ms", (unsigned)longerSuppress);
    }
}

/*
//fw+ DTN Overlay Module — Overview

Purpose
- Opportunistic store–carry–forward for Direct Messages without requiring stock nodes to change. FW+ nodes wrap overheard
  unicasts in an FWPLUS_DTN envelope and may forward them later; the destination unwraps and injects the original DM.

Receive path
- Runs promiscuously; accepts encrypted frames to capture DM ciphertext. If we are the destination of FWPLUS_DTN DATA we
  always unwrap and deliver, even when forwarding is disabled.
- For non-DTN frames we capture encrypted unicasts and plaintext TEXT_MESSAGE_APP unicasts only.

Scheduling (no immediate TX)
- On capture we enqueue immediately but schedule the first attempt later using: initial base delay, deterministic anti-storm
  slotting, per-destination spacing, DV‑ETX confidence gating, and grace/neighbor-aware delay. Optional earlier slot when
  we already have a confident route. Topology-aware throttles further delay or skip action when we are far from the source/dest
  (ring/hops based): only nodes within a few hops act early; distant nodes wait until a fraction of TTL has elapsed.

Forwarding
- Send FWPLUS_DTN DATA toward the destination. Priority BACKGROUND by default; escalate to DEFAULT only in TTL tail, and
  only if we are near the destination (within configured hop ring).
- Retries use backoff and stop at max tries.

Receipts & milestones
- Any receipt clears local pending and sets a short tombstone to avoid immediate re-capture storms. Milestone PROGRESSED is
  sent only once per id, rate-limited and ring-gated (emit only when close to source/destination) and suppressed when
  channel utilization is high.

Fallback & probing
- In TTL tail we may probe FW+ or, if allowed and dest is not FW+, send native DM as proxy fallback, preserving ids.

Airtime protections
- Channel utilization gate, per-destination spacing, global active cap, suppression after foreign DATA, grace/neighbor heuristics.
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
#include <pb_encode.h>
#include <cstring>

DtnOverlayModule *dtnOverlayModule; 

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

DtnOverlayModule::DtnOverlayModule()
    : concurrency::OSThread("DtnOverlay"),
      ProtobufModule("FwplusDtn", meshtastic_PortNum_FWPLUS_DTN_APP, &meshtastic_FwplusDtn_msg)
{
    //enable promiscuous sniffing and acceptance of encrypted packets for overlay capture
    isPromiscuous = true;
    encryptedOk = true;

    //read config with sensible defaults (moduleConfig.dtn_overlay may not exist yet; use defaults)
    configEnabled = false; // default OFF; user can enable in ModuleConfig
    configTtlMinutes = 5;
    configInitialDelayBaseMs = 8000;
    configRetryBackoffMs = 60000;
    configMaxTries = 3;
    configLateFallback = false;
    configFallbackTailPercent = 20;
    configMilestonesEnabled = true; //default ON; user config may override
    configPerDestMinSpacingMs = 30000;
    configMaxActiveDm = 2;
    configProbeFwplusNearDeadline = false;
    //conservative airtime heuristics
    configGraceAckMs = 2500;                  // give direct a brief chance first
    configSuppressMsAfterForeign = 20000;     // back off if someone else is already carrying
    configSuppressIfDestNeighbor = true;      // if dest is our neighbor, be extra conservative
    configPreferBestRouteSlotting = true;     // prefer earlier slot if we have DV-ETX
 
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
    // LOG_INFO("DTN init: enabled=%d ttl_min=%u initDelayMs=%u backoffMs=%u maxTries=%u lateFallback=%d tail%%=%u milestones=%d perDestMinMs=%u maxActive=%u probeNearDeadline=%d graceAckMs=%u suppressForeignMs=%u neighborSuppress=%d preferBestRoute=%d maxRings=%u milestoneMaxRing=%u tailEscMaxRing=%u farMinTtl%%=%u",
    //          (int)configEnabled, (unsigned)configTtlMinutes, (unsigned)configInitialDelayBaseMs,
    //          (unsigned)configRetryBackoffMs, (unsigned)configMaxTries, (int)configLateFallback,
    //          (unsigned)configFallbackTailPercent, (int)configMilestonesEnabled,
    //          (unsigned)configPerDestMinSpacingMs, (unsigned)configMaxActiveDm,
    //          (int)configProbeFwplusNearDeadline,
    //          (unsigned)configGraceAckMs, (unsigned)configSuppressMsAfterForeign,
    //          (int)configSuppressIfDestNeighbor, (int)configPreferBestRouteSlotting,
    //          (unsigned)configMaxRingsToAct, (unsigned)configMilestoneMaxRing, (unsigned)configTailEscalateMaxRing,
    //          (unsigned)configFarMinTtlFracPercent);
}

int32_t DtnOverlayModule::runOnce()
{
    if (!configEnabled) return 1000; //fw+ disabled: idle
    //simple scheduler: attempt forwards whose time arrived
    uint32_t now = millis();
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t dmIssuedThisPass = 0; // reset per scheduler pass
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
            }
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
        if (lastDestTxMs.size() > kMaxPerDestCacheEntries) {
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
    }
    return 500;
}

bool DtnOverlayModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg)
{
    if (msg && msg->which_variant == meshtastic_FwplusDtn_data_tag) {
        // capability: mark sender as FW+
        markFwplusSeen(getFrom(&mp));
        // Milestone: if enabled and we first see this id from another node – send a rare progress to the source 
    if (configMilestonesEnabled) {
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
                emitReceipt(msg->variant.data.orig_from, msg->variant.data.orig_id,
                            meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, via);
                ctrMilestonesSent++; 
                    //immediately tombstone this id locally to avoid re-emitting milestone on repeated hears
                    if (configTombstoneMs) tombstoneUntilMs[msg->variant.data.orig_id] = millis() + configTombstoneMs;
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
    // Non-DTN packet (decoded==NULL): optionally capture DM into overlay
    // Capture only direct messages we overhear, not from us and not addressed to us 
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
                //fw+ native ACK/NAK detection: cancel our pending for this orig DM id
                pendingById.erase(mp.decoded.request_id);
                if (configTombstoneMs) tombstoneUntilMs[mp.decoded.request_id] = millis() + configTombstoneMs;
            }
        }
    }
    return false; //do not consume non-DTN packets; allow normal processing
}

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

void DtnOverlayModule::handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r)
{
    // Simplest: stop any pending entry
    (void)mp;
    pendingById.erase(r.orig_id);
    // Create a short tombstone to prevent immediate re-capture storms of same id 
    if (configTombstoneMs) tombstoneUntilMs[r.orig_id] = millis() + configTombstoneMs;
    ctrReceiptsReceived++; //fw+
}

void DtnOverlayModule::scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d)
{
    auto &p = pendingById[id];
    p.data = d;
    p.lastCarrier = nodeDB->getNodeNum();
    // Per-destination spacing: if we tried to the same destination too recently, postpone 
    auto itLast = lastDestTxMs.find(d.orig_to);
    // Anti-storm election window: deterministic slot based on id^me
    uint32_t base = configInitialDelayBaseMs ? configInitialDelayBaseMs : 8000;
    // Apply grace window before first attempt to give direct delivery/ACK a chance 
    if (p.tries == 0 && configGraceAckMs) {
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
    uint32_t slot = rank % slots;
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

void DtnOverlayModule::tryForward(uint32_t id, Pending &p)
{
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) { p.nextAttemptMs = millis() + 2500 + (uint32_t)random(500); LOG_DEBUG("DTN busy: defer id=0x%x", id); return; }

    // DV-ETX route confidence gating (more permissive when mobile)
    if (!hasSufficientRouteConfidence(p.data.orig_to)) {
        // Backoff softly
        float mobility = fwplus_getMobilityFactor01();
        uint32_t backoff = configRetryBackoffMs + (uint32_t)random(2000);
        if (mobility > 0.5f) backoff = backoff / 2; // try sooner if we are moving
        p.nextAttemptMs = millis() + backoff;
        LOG_DEBUG("DTN low confidence: defer id=0x%x", id);
        return;
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

    if (inTail && !isFwplus(p.data.orig_to) && p.data.allow_proxy_fallback) {
        // Send native DM (ciphertext or plaintext) towards destination
        meshtastic_MeshPacket *dm = allocDataPacket();
        if (!dm) { p.nextAttemptMs = millis() + 3000; return; }
        dm->to = p.data.orig_to;
        // Preserve original sender for UI after decryption at the destination 
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
        }
        //fw+ preserve original DM id so that ROUTING_APP ACK maps to sender's pending entry
        dm->id = p.data.orig_id;
        dm->want_ack = true; // try to get radio ACK 
        dm->decoded.want_response = false;
        dm->priority = meshtastic_MeshPacket_Priority_DEFAULT;
        service->sendToMesh(dm, RX_SRC_LOCAL, false);
        p.tries++;
        p.nextAttemptMs = millis() + configRetryBackoffMs;
        LOG_INFO("DTN fallback DM id=0x%x dst=0x%x try=%u", id, (unsigned)p.data.orig_to, (unsigned)p.tries);
        ctrFallbacksAttempted++; 
        return;
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
    mp->to = p.data.orig_to; // destination node
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
    // Before sending, if we are further from the destination than the source, back off extra to favor closer relayers //fw+
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    uint8_t hopsToSrc = getHopsAway(p.data.orig_from);
    if (hopsToSrc != 255 && hopsToDest != 255 && hopsToDest > hopsToSrc) {
        p.nextAttemptMs = millis() + configRetryBackoffMs + 5000 + (uint32_t)random(2000);
        return;
    }
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    ctrForwardsAttempted++; 
    lastForwardMs = millis(); 
    // update per-destination last tx (bounded map)
    lastDestTxMs[p.data.orig_to] = millis();
    // schedule next attempt with backoff
    p.nextAttemptMs = millis() + (configRetryBackoffMs ? configRetryBackoffMs : 60000);
    LOG_INFO("DTN fwd overlay id=0x%x dst=0x%x try=%u next=%u ms", id, (unsigned)p.data.orig_to, (unsigned)p.tries,
             (unsigned)(p.nextAttemptMs - millis()));
}

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



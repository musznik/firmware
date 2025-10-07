#include "FloodingRouter.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "configuration.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif

#include "MeshService.h"
#include "NodeDB.h"
#include "gps/GeoCoord.h"

//fw+ forward declare BroadcastAssist to avoid depending on module headers
class BroadcastAssistModule;
extern BroadcastAssistModule *broadcastAssistModule;

FloodingRouter::FloodingRouter() {
    // Initialize profile defaults (no designated initializers to keep C++11 compatible)
    profileParamsSparse.base = 10;
    profileParamsSparse.hop = 0;
    profileParamsSparse.snrGain = 6;
    profileParamsSparse.jitter = 10;
    profileParamsSparse.backboneBias = 0;

    profileParamsBalanced.base = 60;
    profileParamsBalanced.hop = 30;
    profileParamsBalanced.snrGain = 8;
    profileParamsBalanced.jitter = 40;
    profileParamsBalanced.backboneBias = 0;

    profileParamsDense.base = 120;
    profileParamsDense.hop = 50;
    profileParamsDense.snrGain = 10;
    profileParamsDense.jitter = 80;
    profileParamsDense.backboneBias = 0;

    profileParamsBridge.base = 60;
    profileParamsBridge.hop = 30;
    profileParamsBridge.snrGain = 8;
    profileParamsBridge.jitter = 40;
    profileParamsBridge.backboneBias = 20;
}

/**
 * Send a packet on a suitable interface.  This routine will
 * later free() the packet to pool.  This routine is not allowed to stall.
 * If the txmit queue is full it might return an error
 */
ErrorCode FloodingRouter::send(meshtastic_MeshPacket *p)
{
    flood_counter++;
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    return Router::send(p);
}

bool FloodingRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    bool wasUpgraded = false;
    bool seenRecently =
        wasSeenRecently(p, true, nullptr, nullptr, &wasUpgraded); // Updates history; returns false when an upgrade is detected

    // Handle hop_limit upgrade scenario for rebroadcasters
    if (wasUpgraded && perhapsHandleUpgradedPacket(p)) {
        return true; // we handled it, so stop processing
    }

    if (seenRecently) {
        printPacket("Ignore dupe incoming msg", p);
        rxDupe++;
        //fw+ notify BroadcastAssist about upstream duplicate drops (by sender+id) without including module headers
        extern void fwplus_ba_onOverheardFromId(uint32_t from, uint32_t id);
        fwplus_ba_onOverheardFromId(getFrom(p), p->id);

        /* If the original transmitter is doing retransmissions (hopStart equals hopLimit) for a reliable transmission, e.g., when
        the ACK got lost, we will handle the packet again to make sure it gets an implicit ACK. */
        bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
        if (isRepeated) {
            LOG_DEBUG("Repeated reliable tx");
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id)) {
                reprocessPacket(p);
                perhapsRebroadcast(p);
            }
        } else {
            perhapsCancelDupe(p);
        }

        return true;
    }

    return Router::shouldFilterReceived(p);
}

bool FloodingRouter::perhapsHandleUpgradedPacket(const meshtastic_MeshPacket *p)
{
    // isRebroadcaster() is duplicated in perhapsRebroadcast(), but this avoids confusing log messages
    if (isRebroadcaster() && iface && p->hop_limit > 0) {
        // If we overhear a duplicate copy of the packet with more hops left than the one we are waiting to
        // rebroadcast, then remove the packet currently sitting in the TX queue and use this one instead.
        uint8_t dropThreshold = p->hop_limit; // remove queued packets that have fewer hops remaining
        if (iface->removePendingTXPacket(getFrom(p), p->id, dropThreshold)) {
            LOG_DEBUG("Processing upgraded packet 0x%08x for rebroadcast with hop limit %d (dropping queued < %d)", p->id,
                      p->hop_limit, dropThreshold);

            reprocessPacket(p);
            perhapsRebroadcast(p);

            rxDupe++;
            // We already enqueued the improved copy, so make sure the incoming packet stops here.
            return true;
        }
    }

    return false;
}

void FloodingRouter::reprocessPacket(const meshtastic_MeshPacket *p)
{
    if (nodeDB)
        nodeDB->updateFrom(*p);
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
    if (traceRouteModule && p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP)
        traceRouteModule->processUpgradedPacket(*p);
#endif
}

bool FloodingRouter::roleAllowsCancelingDupe(const meshtastic_MeshPacket *p)
{
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
        config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
        // ROUTER, ROUTER_LATE should never cancel relaying a packet (i.e. we should always rebroadcast),
        // even if we've heard another station rebroadcast it already.
        return false;
    }

    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
        // CLIENT_BASE: if the packet is from or to a favorited node,
        // we should act like a ROUTER and should never cancel a rebroadcast (i.e. we should always rebroadcast),
        // even if we've heard another station rebroadcast it already.
        return !nodeDB->isFromOrToFavoritedNode(*p);
    }

    // All other roles (such as CLIENT) should cancel a rebroadcast if they hear another station's rebroadcast.
    return true;
}

void FloodingRouter::perhapsCancelDupe(const meshtastic_MeshPacket *p)
{
    //fw+ prefer opportunistic cancel on first-hear; fall back to role-based cancel
    bool allowCancel = (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA);
    if (allowCancel) {
        if (isOpportunisticEnabled() && moduleConfig.nodemodadmin.opportunistic_cancel_on_first_hear) {
            if (Router::cancelSending(p->from, p->id)) {
                txRelayCanceled++;
                opportunistic_canceled++;
            }
        } else if (roleAllowsCancelingDupe(p)) {
            if (Router::cancelSending(p->from, p->id))
                txRelayCanceled++;
        }
    }
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE && iface) {
        iface->clampToLateRebroadcastWindow(getFrom(p), p->id);
    }
}

bool FloodingRouter::isRebroadcaster()
{
    return config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE &&
           config.device.rebroadcast_mode != meshtastic_Config_DeviceConfig_RebroadcastMode_NONE;
}

bool FloodingRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
{
    //fw+
    if (!isToUs(p) && (p->hop_limit <= 0) && !isFromUs(p)) {
        blocked_by_hoplimit++;
    }

    if (!isToUs(p) && (p->hop_limit > 0) && !isFromUs(p)) {
        if (p->id != 0) {
            if (isRebroadcaster()) {
                // If telemetry limiter is enabled and this is TELEMETRY_APP, enforce per-minute limit
                if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
                    p->decoded.portnum == meshtastic_PortNum_TELEMETRY_APP) {
                    if (!isTelemetryRebroadcastLimited(p)) {
                        return false; // limit reached: do not rebroadcast telemetry
                    }
                }
                // If position limiter is enabled and this is POSITION_APP broadcast with unchanged position within threshold, block
                if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
                    p->decoded.portnum == meshtastic_PortNum_POSITION_APP) {
                    if (!isPositionRebroadcastAllowed(p)) {
                        return false; // too frequent unchanged position broadcast: do not rebroadcast
                    }
                }
                meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p); // keep a copy because we will be sending it

                // Use shared logic to determine if hop_limit should be decremented
                if (shouldDecrementHopLimit(p)) {
                    tosend->hop_limit--; // bump down the hop count
                } else {
                    LOG_INFO("favorite-ROUTER/CLIENT_BASE-to-ROUTER/CLIENT_BASE flood: preserving hop_limit");
                }
#if USERPREFS_EVENT_MODE
                if (tosend->hop_limit > 2) {
                    // if we are "correcting" the hop_limit, "correct" the hop_start by the same amount to preserve hops away.
                    tosend->hop_start -= (tosend->hop_limit - 2);
                    tosend->hop_limit = 2;
                }
#endif

                tosend->next_hop = NO_NEXT_HOP_PREFERENCE; // this should already be the case, but just in case

                LOG_INFO("Rebroadcast received floodmsg");
                // Note: we are careful to resend using the original senders node id
                // We are careful not to call our hooked version of send() - because we don't want to check this again
                // Opportunistic/selective flooding: schedule delayed rebroadcast, cancel on hear
                if (isOpportunisticEnabled() && isBroadcast(tosend->to)) {
                    uint32_t delayMs = computeOpportunisticDelayMs(p);
                    if (delayMs > 0) {
                        tosend->tx_after = millis() + delayMs;
                    }
                    opportunistic_scheduled++;
                }

                Router::send(tosend);
                return true;
            } else {
                LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
            }
        } else {
            LOG_DEBUG("Ignore 0 id broadcast");
        }
    }
    return false;
}

void FloodingRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply && !isToUs(p) && !isBroadcast(p->to)) {
        // do not flood direct message that is ACKed or replied to
        LOG_DEBUG("Rxd an ACK/reply not for me, cancel rebroadcast");
        Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
    }

    perhapsRebroadcast(p);

    // handle the packet as normal
    Router::sniffReceived(p, c);

    // Opportunistic profile: observe RX and maybe recompute every window //fw+
    observeRxForProfile(p);
    maybeRecomputeProfile(millis());
}

void FloodingRouter::observeRxForProfile(const meshtastic_MeshPacket *p)
{
    // Initialize window if needed
    if (profileWindowStartMs == 0) {
        profileWindowStartMs = millis();
        lastRxDupeCounter = rxDupe;
        chanUtilEma = airTime->channelUtilizationPercent();
    }

    // Track non-duplicate broadcast count in this window
    if (isBroadcast(p->to)) {
        // wasSeenRecently()  sniffReceived rcvd nonduplicates?
        windowBroadcastNonDup++;
    }

    // Smooth channel utilization
    float util = airTime->channelUtilizationPercent();
    const float alpha = 0.2f;
    chanUtilEma = (1.0f - alpha) * chanUtilEma + alpha * util;
}

void FloodingRouter::maybeRecomputeProfile(uint32_t nowMs)
{
    if (profileWindowStartMs == 0) return;
    if (nowMs - profileWindowStartMs < profileWindowMs) return;

    // Compute window metrics
    uint32_t dupes = rxDupe - lastRxDupeCounter;
    float dupeRatio = 0.0f;
    if (windowBroadcastNonDup > 0) {
        dupeRatio = (float)dupes / (float)windowBroadcastNonDup;
    }

    // Estimate neighbors 1-hop (non-MQTT)
    neighborsWin = 0;
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total; ++i) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n) continue;
        if (n->hops_away == 0 && !n->via_mqtt && n->num != nodeDB->getNodeNum()) neighborsWin++;
    }

    // Select target profile with simple thresholds; BACKBONE_BRIDGE overrides
    OpportunisticProfile newTarget = OpportunisticProfile::BALANCED;
    bool hasBackbone = hasBackboneNeighbor();
    if (hasBackbone) {
        newTarget = OpportunisticProfile::BACKBONE_BRIDGE;
    } else if (neighborsWin <= 1 && dupeRatio < 0.05f && chanUtilEma < 10.0f) {
        newTarget = OpportunisticProfile::SPARSE;
    } else if (neighborsWin >= 4 || dupeRatio >= 0.20f || chanUtilEma >= 30.0f) {
        newTarget = OpportunisticProfile::DENSE;
    }

    if (newTarget == targetProfile) {
        stableWindows++;
    } else {
        targetProfile = newTarget;
        stableWindows = 0;
    }

    if (stableWindows >= requiredStableWindows) {
        currentProfile = targetProfile;
    }

    // Reset window
    profileWindowStartMs = nowMs;
    lastRxDupeCounter = rxDupe;
    windowBroadcastNonDup = 0;
}

const FloodingRouter::ProfileParams &FloodingRouter::getParamsFor(OpportunisticProfile p) const
{
    switch (p) {
        case OpportunisticProfile::SPARSE: return profileParamsSparse;
        case OpportunisticProfile::DENSE: return profileParamsDense;
        case OpportunisticProfile::BACKBONE_BRIDGE: return profileParamsBridge;
        case OpportunisticProfile::BALANCED:
        default: return profileParamsBalanced;
    }
}

//fw+
bool FloodingRouter::isOpportunisticEnabled() const
{
    if (!moduleConfig.has_nodemodadmin)
        return false;
    return moduleConfig.nodemodadmin.opportunistic_flooding_enabled;
}

bool FloodingRouter::isOpportunisticAuto() const
{
    if (!moduleConfig.has_nodemodadmin)
        return true; // default to auto when section missing
    return moduleConfig.nodemodadmin.opportunistic_auto;
}

//fw+
uint32_t FloodingRouter::clampDelay(uint32_t d) const
{
    // Reasonable defaults; can be adjusted later or made configurable
    const uint32_t D_MIN = 5;
    const uint32_t D_MAX = 800;
    if (d < D_MIN) return D_MIN;
    if (d > D_MAX) return D_MAX;
    return d;
}

//fw+
uint32_t FloodingRouter::computeOpportunisticDelayMs(const meshtastic_MeshPacket *p) const
{
    if (!moduleConfig.has_nodemodadmin)
        return 0;

    const auto &adm = moduleConfig.nodemodadmin;
    uint32_t base = adm.opportunistic_base_delay_ms;
    uint32_t hop = adm.opportunistic_hop_delay_ms;
    uint32_t snrGain = adm.opportunistic_snr_gain_ms;
    uint32_t jitter = adm.opportunistic_jitter_ms;

    // if no parameters set, skip
    if (base == 0 && hop == 0 && snrGain == 0 && jitter == 0)
        return 0;

     // compute used hops if hop_start is present
    int32_t usedHops = 0;
    if (p->hop_start > 0) {
        usedHops = (int32_t)p->hop_start - (int32_t)p->hop_limit;
        if (usedHops < 0) usedHops = 0;
    }

    // Clamp SNR into [0, 20] for scaling benefit only for positive SNR
    float snr = p->rx_snr;
    if (snr < 0) snr = 0;
    if (snr > 20) snr = 20;

    // Random jitter [0..jitter]?
    uint32_t rj = jitter ? (random(jitter + 1)) : 0;

    int32_t d = 0;
    if (isOpportunisticAuto()) {
        const ProfileParams &pp = getParamsFor(currentProfile);
        uint16_t pBase = pp.base;
        uint16_t pHop = pp.hop;
        uint8_t pSnr = pp.snrGain;
        int32_t backboneBias = (int32_t)pp.backboneBias;
        d = (int32_t)pBase + (int32_t)(pHop * usedHops) - (int32_t)(pSnr * (int32_t)snr) + (int32_t)rj - backboneBias;
    } else {
        // Manual override path (admin-config values)
        uint16_t pBase = base ? base : 60;
        uint16_t pHop = hop ? hop : 30;
        uint8_t pSnr = snrGain ? (uint8_t)snrGain : 8;
        d = (int32_t)pBase + (int32_t)(pHop * usedHops) - (int32_t)(pSnr * (int32_t)snr) + (int32_t)rj;
    }
    if (d <= 0) return (uint32_t)rj; // ensure non-negative, allow pure jitter
    return clampDelay((uint32_t)d);
}

//fw+
bool FloodingRouter::hasBackboneNeighbor() const
{
    //heuristic: if we have a 1-hop neighbor with a large distance ==> treat it as backbone
    //needs both positions: ours and neighbor’s, if missing  fail-open: false
    const meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!self || !nodeDB->hasValidPosition(self)) return false;

    double selfLat = self->position.latitude_i * 1e-7;
    double selfLon = self->position.longitude_i * 1e-7;

    // distance threshold: 10 km (configurable in the future), backbone?
    const float thresholdMeters = 10000.0f;

    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total; ++i) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == self->num) continue;
        if (n->hops_away != 0 || n->via_mqtt) continue; //only direct neighbors
        if (!nodeDB->hasValidPosition(n)) continue;

        double nLat = n->position.latitude_i * 1e-7;
        double nLon = n->position.longitude_i * 1e-7;
        float dist = GeoCoord::latLongToMeter(selfLat, selfLon, nLat, nLon);
        if (dist >= thresholdMeters) return true;
    }
    return false;
}
bool FloodingRouter::isTelemetryRebroadcastLimited(const meshtastic_MeshPacket *p)
{
    // Read admin config from global moduleConfig
    if (!moduleConfig.has_nodemodadmin) return true; // no admin module present, allow

    const bool limiterEnabled = moduleConfig.nodemodadmin.telemetry_limiter_enabled;
    const bool autoChanutilEnabled = moduleConfig.nodemodadmin.telemetry_limiter_auto_chanutil_enabled;
    const uint16_t limitPerMinute = moduleConfig.nodemodadmin.telemetry_limiter_packets_per_minute;
    const uint32_t chanUtilThreshold = moduleConfig.nodemodadmin.telemetry_limiter_auto_chanutil_threshold;

    // Master switch off: allow all
    if (!limiterEnabled) return true;

    // If auto mode is enabled, enforce limiter only when channel utilization exceeds threshold
    if (autoChanutilEnabled) {
        float currentChanUtil = airTime->channelUtilizationPercent();
        if (currentChanUtil < (float)chanUtilThreshold) {
            return true; // below threshold: bypass limiter
        }
    }

    // Enforce per-minute limiter
    const uint32_t now = millis();
    if (now - telemetryWindowStartMs >= 60000UL) {
        telemetryWindowStartMs = now;
        telemetryPacketsInWindow = 0;
    }

    if (telemetryPacketsInWindow >= limitPerMinute) {
        LOG_DEBUG("Telemetry limiter: limit reached (%u/min)", limitPerMinute);
        return false; // do not allow
    }

    // count only when we actually rebroadcast (we're about to)
    telemetryPacketsInWindow++;
    return true;
}

bool FloodingRouter::isPositionRebroadcastAllowed(const meshtastic_MeshPacket *p)
{
    if (!moduleConfig.has_nodemodadmin) return true;
    if (!moduleConfig.nodemodadmin.position_limiter_enabled) return true;
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) return true;
    if (p->decoded.portnum != meshtastic_PortNum_POSITION_APP) return true;
    if (!isBroadcast(p->to) || isFromUs(p)) return true; // only throttle broadcast relays of others

    meshtastic_Position pos = meshtastic_Position_init_default;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_Position_msg, &pos)) {
        return true; // if we can't decode, fail-open
    }

    // If position coordinates are not present (0,0) but that's uncommon; still apply logic directly
    uint32_t thresholdMin = moduleConfig.nodemodadmin.position_limiter_time_minutes_threshold;
    if (thresholdMin == 0) return true; // unset threshold means no throttling
    uint32_t thresholdMs = thresholdMin * 60000UL;
    uint32_t now = millis();

    // Find existing entry
    for (size_t i = 0; i < recentForwardedPositionsCount; ++i) {
        if (recentForwardedPositions[i].nodeId == p->from) {
            bool sameCoords = (recentForwardedPositions[i].lastLat_i == pos.latitude_i) &&
                              (recentForwardedPositions[i].lastLon_i == pos.longitude_i);
            if (sameCoords && (now - recentForwardedPositions[i].lastRebroadcastMs) < thresholdMs) {
                // Still within threshold with unchanged position → block
                return false;
            }
            // Update LRU / values and allow
            upsertPositionEntryLRU(p->from, pos.latitude_i, pos.longitude_i, now);
            return true;
        }
    }

    // No existing entry → add and allow
    upsertPositionEntryLRU(p->from, pos.latitude_i, pos.longitude_i, now);
    return true;
}

void FloodingRouter::upsertPositionEntryLRU(uint32_t nodeId, int32_t lat_i, int32_t lon_i, uint32_t nowMs)
{
    // Find existing index
    size_t found = kMaxPositionEntries;
    for (size_t i = 0; i < recentForwardedPositionsCount; ++i) {
        if (recentForwardedPositions[i].nodeId == nodeId) { found = i; break; }
    }

    ForwardedPositionEntry entry;
    entry.nodeId = nodeId;
    entry.lastLat_i = lat_i;
    entry.lastLon_i = lon_i;
    entry.lastRebroadcastMs = nowMs;

    if (found < kMaxPositionEntries) {
        // Shift down to make room at front
        for (size_t i = found; i > 0; --i) {
            recentForwardedPositions[i] = recentForwardedPositions[i - 1];
        }
        recentForwardedPositions[0] = entry;
    } else {
        // Insert new at front
        size_t limit = recentForwardedPositionsCount < kMaxPositionEntries ? recentForwardedPositionsCount : (kMaxPositionEntries - 1);
        // Shift right up to limit
        for (size_t i = limit; i > 0; --i) {
            recentForwardedPositions[i] = recentForwardedPositions[i - 1];
        }
        recentForwardedPositions[0] = entry;
        if (recentForwardedPositionsCount < kMaxPositionEntries) recentForwardedPositionsCount++;
    }
}

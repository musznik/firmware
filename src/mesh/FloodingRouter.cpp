#include "FloodingRouter.h"

#include "configuration.h"
#include "mesh-pb-constants.h"

#include "MeshService.h"
#include "NodeDB.h"
#include "gps/GeoCoord.h"

FloodingRouter::FloodingRouter() {}

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
    if (wasSeenRecently(p)) { // Note: this will also add a recent packet record
        printPacket("Ignore dupe incoming msg", p);
        rxDupe++;

        /* If the original transmitter is doing retransmissions (hopStart equals hopLimit) for a reliable transmission, e.g., when
        the ACK got lost, we will handle the packet again to make sure it gets an implicit ACK. */
        bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
        if (isRepeated) {
            LOG_DEBUG("Repeated reliable tx");
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id))
                perhapsRebroadcast(p);
        } else {
            perhapsCancelDupe(p);
        }

        return true;
    }

    return Router::shouldFilterReceived(p);
}

void FloodingRouter::perhapsCancelDupe(const meshtastic_MeshPacket *p)
{
    bool allowCancel = (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA);
    if (allowCancel) {
        if (isOpportunisticEnabled() && moduleConfig.nodemodadmin.cancel_on_first_hear) {
            if (Router::cancelSending(p->from, p->id)) {
                txRelayCanceled++;
                opportunistic_canceled++;
            }
        } else if (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER &&
                   config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER &&
                   config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
            // legacy behavior?
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

void FloodingRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
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
                        return; // limit reached: do not rebroadcast telemetry
                    }
                }
                // If position limiter is enabled and this is POSITION_APP broadcast with unchanged position within threshold, block
                if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
                    p->decoded.portnum == meshtastic_PortNum_POSITION_APP) {
                    if (!isPositionRebroadcastAllowed(p)) {
                        return; // too frequent unchanged position broadcast: do not rebroadcast
                    }
                }
                meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p); // keep a copy because we will be sending it

                tosend->hop_limit--; // bump down the hop count
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
            } else {
                LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
            }
        } else {
            LOG_DEBUG("Ignore 0 id broadcast");
        }
    }
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
}

//fw+
bool FloodingRouter::isOpportunisticEnabled() const
{
    if (!moduleConfig.has_nodemodadmin)
        return false;
    return moduleConfig.nodemodadmin.opportunistic_flooding_enabled;
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
    uint32_t base = adm.base_delay_ms;
    uint32_t hop = adm.hop_delay_ms;
    uint32_t snrGain = adm.snr_gain_ms;
    uint32_t jitter = adm.jitter_ms;

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

    // Optional backbone bias: if we have a rare/distant neighbor, transmit a bit earlier
    int32_t backboneBias = hasBackboneNeighbor() ? 20 : 0; // ms, small but helps win race

    // D = base + hop*usedHops - snrGain*snr + jitter - backboneBias
    int32_t d = (int32_t)base + (int32_t)(hop * usedHops) - (int32_t)(snrGain * (int32_t)snr) + (int32_t)rj - backboneBias;
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

    // distance threshold: 5 km (configurable in the future)
    const float thresholdMeters = 5000.0f;

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
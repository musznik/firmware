#include "NextHopRouter.h"
#include "MeshService.h"
#include <pb.h>
#include <pb_decode.h>
#include "MeshTypes.h"
#include "meshUtils.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif
#include "NodeDB.h"
#include "MobilityOracle.h" //fw+

//fw+ forward declare BroadcastAssist to avoid depending on module headers
class BroadcastAssistModule;
extern BroadcastAssistModule *broadcastAssistModule;

NextHopRouter::NextHopRouter() {}
//fw++
// Purpose: DV-ETX route learning from traceroute paths
// Supports both ACTIVE learning (we're in path) and PASSIVE learning (we relay foreign traceroute)
// This enables faster routing table buildup in large networks (100-200 nodes)
void NextHopRouter::processPathAndLearn(const uint32_t *path, size_t maxHops,
                                        const int8_t *snrList, size_t maxSnr,
                                        const meshtastic_MeshPacket *p)
{
    if (!path || maxHops < 2) {
        LOG_DEBUG("NextHop: processPathAndLearn guard: path=%p maxHops=%u", path, (unsigned)maxHops);
        return;
    }
    uint32_t selfNum = getNodeNum();
    int selfIdx = -1;
    for (size_t i = 0; i < maxHops; ++i) {
        if (path[i] == selfNum) { selfIdx = (int)i; break; }
    }
    
    // ═══════════════════════════════════════════════════════════════════
    // DV-ETX ACTIVE LEARNING: We are in path (high confidence)
    // ═══════════════════════════════════════════════════════════════════
    if (selfIdx >= 0 && (selfIdx + 1) < (int)maxHops) {
        uint32_t dest = path[maxHops - 1];
        uint8_t nextHop = (uint8_t)(path[selfIdx + 1] & 0xFF);

        // Gate on direct neighbor presence for the candidate last byte
        if (!isDirectNeighborLastByte(nextHop)) {
            LOG_DEBUG("NextHop: Skip ACTIVE learn: nextHop 0x%x is not a direct neighbor", nextHop);
            return;  // Can't learn if next hop isn't our direct neighbor
        }

        float linkEtx = 2.0f;
        if (snrList && maxSnr > (size_t)selfIdx) {
            float snr = (float)snrList[selfIdx] / 4.0f;
            linkEtx = estimateEtxFromSnr(snr);
        } else if (p) {
            linkEtx = estimateEtxFromSnr(p->rx_snr);
        } else {
            linkEtx = 1.5f; // Default conservative ETX when no SNR data available (DTN custody chains)
        }
        int remainingHops = (int)maxHops - selfIdx - 1;
        float observedCost = linkEtx + (remainingHops > 1 ? (remainingHops - 1) * 1.0f : 0.0f);
        learnRoute(dest, nextHop, observedCost);
        LOG_INFO("NextHop: ACTIVE LEARN dest=0x%x via=0x%x cost=%.1f SNR=%.1f pathlen=%u pos=%d [IN-PATH TRACEROUTE]", 
                 dest, nextHop, observedCost, linkEtx, (unsigned)maxHops, selfIdx);
    }
    // ═══════════════════════════════════════════════════════════════════
    // DV-ETX PASSIVE LEARNING: We relay foreign traceroute (opportunistic)
    // ═══════════════════════════════════════════════════════════════════
    // If we're NOT in path but relaying this traceroute, we can infer:
    // "destination is reachable via the neighbor who sent us this traceroute"
    // This dramatically speeds up route discovery in large networks (100-200 nodes)
    else if (selfIdx < 0 && maxHops >= 2) {
        //fw+ CRITICAL: Check if p is not nullptr before accessing (DTN custody chains pass nullptr)
        if (!p) {
            LOG_DEBUG("NextHop: Skip PASSIVE learn: no packet context (DTN custody chain, need packet for receivedFrom)");
            return;
        }
        
        uint32_t dest = path[maxHops - 1];  // Final destination
        uint32_t receivedFrom = getFrom(p);  // Who sent us this packet
        uint8_t viaHop = (uint8_t)(receivedFrom & 0xFF);
        
        LOG_DEBUG("NextHop: PASSIVE candidate: dest=0x%x via=0x%x (from=0x%x) selfIdx=%d", 
                  dest, viaHop, receivedFrom, selfIdx);
        
        // Only learn if receivedFrom is our direct neighbor
        if (!isDirectNeighborLastByte(viaHop)) {
            LOG_DEBUG("NextHop: Skip PASSIVE learn: viaHop 0x%x is not a direct neighbor", viaHop);
            return;  // Can't use as next hop if not direct neighbor
        }
        
        // ETX calculation for our link to the neighbor (same as active)
        float linkEtx = estimateEtxFromSnr(p->rx_snr);
        
        // DV cost estimation: link ETX + conservative path estimate
        // We use path length + small penalty for uncertainty
        float estimatedCost = linkEtx + (float)(maxHops - 1) * 1.1f;
        
        // Update DV-ETX routing table with passive observation
        // Note: learnRoute() internally uses EMA smoothing and confidence tracking
        learnRoute(dest, viaHop, estimatedCost);
        LOG_INFO("NextHop: PASSIVE LEARN dest=0x%x via=0x%x cost=%.1f SNR=%.1f pathlen=%u [RELAY DISCOVERY]", 
                 dest, viaHop, estimatedCost, p->rx_snr, (unsigned)maxHops);
    }
}
//fw+
void NextHopRouter::learnFromRouteDiscoveryPayload(const meshtastic_MeshPacket *p)
{
    if (!p) {
        LOG_WARN("NextHop: learnFromRouteDiscoveryPayload called with NULL packet");
        return;
    }
    
    meshtastic_RouteDiscovery rd = meshtastic_RouteDiscovery_init_zero;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_RouteDiscovery_msg, &rd)) {
        LOG_WARN("NextHop: Failed to decode RouteDiscovery from packet id=0x%x", p->id);
        return;
    }

    const size_t maxHops = rd.route_back_count ? rd.route_back_count : rd.route_count;
    const uint32_t *path = rd.route_back_count ? rd.route_back : rd.route;
    const size_t maxSnr = rd.snr_back_count ? rd.snr_back_count : rd.snr_towards_count;
    const int8_t *snrList = rd.snr_back_count ? rd.snr_back : rd.snr_towards;
    
    LOG_DEBUG("NextHop: learnFromRouteDiscoveryPayload id=0x%x hops=%u path_type=%s", 
              p->id, (unsigned)maxHops, rd.route_back_count ? "back" : "forward");
    
    //fw+ Skip passive learning for traceroute responses (isToUs) to avoid reversed path bug
    if (!isToUs(p)) {
        processPathAndLearn(path, maxHops, snrList, maxSnr, p);
    }
    
    if (isToUs(p) && rd.route_count > 0) {
        uint8_t firstHop = (uint8_t)(rd.route[0] & 0xFF);
        if (isDirectNeighborLastByte(firstHop)) {
            uint32_t destNode = rd.route[rd.route_count - 1];
            float linkEtx = estimateEtxFromSnr(p->rx_snr);
            if (rd.snr_towards_count > 0 && rd.snr_towards[0] != INT8_MIN) {
                linkEtx = estimateEtxFromSnr((float)rd.snr_towards[0] / 4.0f);
            }
            int remainingHops = (int)rd.route_count;
            float observedCost = linkEtx + (remainingHops > 1 ? (remainingHops - 1) * 1.0f : 0.0f);
            learnRoute(destNode, firstHop, observedCost);
        } else {
            // Fallback: if the declared first hop is not our direct neighbor, seed via the actual neighbor that delivered this packet
            uint8_t viaHop = p->relay_node;
            if (viaHop != 0 && isDirectNeighborLastByte(viaHop)) {
                uint32_t destNode = rd.route[rd.route_count - 1];
                float linkEtx = estimateEtxFromSnr(p->rx_snr);
                int remainingHops = (int)rd.route_count;
                float observedCost = linkEtx + (remainingHops > 1 ? (remainingHops - 1) * 1.0f : 0.0f);
                learnRoute(destNode, viaHop, observedCost);
                LOG_INFO("NextHop: FALLBACK LEARN (traceroute) dest=0x%x via=0x%x cost=%.1f [RELAY NODE]",
                         destNode, viaHop, observedCost);
            }
        }
    }
}
//fw+
void NextHopRouter::learnFromDtnCustodyPath(const uint32_t *path, size_t pathLen)
{
    if (!path || pathLen < 2) {
        LOG_DEBUG("NextHop: learnFromDtnCustodyPath called with invalid path (len=%u)", (unsigned)pathLen);
        return;
    }
    
    // Call processPathAndLearn with DTN custody chain
    // No SNR data available from custody chains, so we pass nullptr
    processPathAndLearn(path, pathLen, nullptr, 0, nullptr);
    
    LOG_INFO("NextHop: Learned from DTN custody chain (len=%u)", (unsigned)pathLen);

    //fw+ If we are the final recipient of this custody hop, the chain does not include 'self'.
    // In that case we can still learn a reliable reverse route TO THE SOURCE via the last carrier
    // (which must be our bezpośredni sąsiad if we just received it).
    // This accelerates building a next hop for replies and reduces flooding.
    {
        uint32_t selfNum = getNodeNum();
        bool selfInPath = false;
        for (size_t i = 0; i < pathLen; ++i) {
            if (path[i] == selfNum) { selfInPath = true; break; }
        }
        if (!selfInPath) {
            // Last carrier before us (8-bit last byte)
            uint8_t viaHop = (uint8_t)(path[pathLen - 1] & 0xFF);
            if (isDirectNeighborLastByte(viaHop)) {
                // Source is the first element of custody chain
                uint32_t sourceNode = path[0];
                // Conservative cost estimate without SNR
                float observedCost = 1.5f + (pathLen > 1 ? (float)(pathLen - 1) * 1.0f : 0.0f);
                learnRoute(sourceNode, viaHop, observedCost);
                LOG_INFO("NextHop: REVERSE LEARN (DTN) src=0x%x via=0x%x cost=%.1f [CUSTODY LAST HOP]",
                         sourceNode, viaHop, observedCost);
            } else {
                LOG_DEBUG("NextHop: Skip REVERSE LEARN (DTN): last carrier 0x%x is not a direct neighbor", viaHop);
            }
        }
    }
}
//fw+
void NextHopRouter::learnFromRoutingPayload(const meshtastic_MeshPacket *p)
{
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_Routing_msg, &routing)) return;
    const meshtastic_RouteDiscovery *rd = NULL;
    if (routing.which_variant == meshtastic_Routing_route_reply_tag) rd = &routing.route_reply;
    else if (routing.which_variant == meshtastic_Routing_route_request_tag) rd = &routing.route_request;
    if (!rd) return;

    const size_t maxHops = rd->route_back_count ? rd->route_back_count : rd->route_count;
    const uint32_t *path = rd->route_back_count ? rd->route_back : rd->route;
    const size_t maxSnr = rd->snr_back_count ? rd->snr_back_count : rd->snr_towards_count;
    const int8_t *snrList = rd->snr_back_count ? rd->snr_back : rd->snr_towards;
    processPathAndLearn(path, maxHops, snrList, maxSnr, p);
}
//fw+ dv-etx
bool NextHopRouter::lookupRoute(uint32_t dest, RouteEntry &out)
{
    auto it = routes.find(dest);
    if (it == routes.end()) return false;
    const RouteEntry &r = it->second;
    // TTL (adaptive) and minimal confidence gate //fw+
    uint32_t now = millis();
    uint32_t ttlMs = computeRouteTtlMs(r.confidence);
    // fw+ shrink effective TTL when mobile (factor 0..1 => cut up to 80%)
    float mobility = fwplus_getMobilityFactor01();
    uint32_t ttlAdj = (uint32_t)((float)ttlMs * (1.0f - 0.8f * mobility));
    if (now - r.lastUpdatedMs > ttlAdj) return false;
    // fw+ lower confidence gate slightly for mobile
    uint8_t minConf = getMinConfidenceToUse();
    if (mobility > 0.5f && minConf > 0) minConf -= 1;
    if (r.confidence < minConf) return false;
    out = r;
    return (out.next_hop != NO_NEXT_HOP_PREFERENCE);
}
//fw+ dv-etx
void NextHopRouter::learnRoute(uint32_t dest, uint8_t viaHop, float observedCost)
{
    if (viaHop == NO_NEXT_HOP_PREFERENCE) return;
    // Safety: never learn a route via a non-direct neighbor
    if (!isDirectNeighborLastByte(viaHop)) return;
    RouteEntry &r = routes[dest];
    if (r.confidence == 0) {
        r.aggregated_cost = observedCost;
    } else {
        // Smooth update with mobility-aware alpha //fw+
        float mobility = fwplus_getMobilityFactor01();
        float alphaNew = 0.3f + mobility * (0.7f - 0.3f); // 0.3..0.7
        r.aggregated_cost = (1.0f - alphaNew) * r.aggregated_cost + alphaNew * observedCost;
    }
    // Update backup if the new candidate is different and better than current
    if (r.next_hop != viaHop) {
        if (r.next_hop == NO_NEXT_HOP_PREFERENCE) {
            r.next_hop = viaHop;
        } else {
            // Decide which is primary/backup by cost
            if (observedCost < (r.aggregated_cost - getHysteresisThreshold())) {
                // Promote new as primary, demote old to backup
                r.backup_next_hop = r.next_hop;
                r.backup_cost = r.aggregated_cost;
                r.next_hop = viaHop;
                r.aggregated_cost = observedCost;
            } else {
                // Keep current primary, maybe store/upgrade backup
                if (r.backup_next_hop == NO_NEXT_HOP_PREFERENCE || observedCost < r.backup_cost || r.backup_next_hop == viaHop) {
                    r.backup_next_hop = viaHop;
                    r.backup_cost = observedCost;
                }
            }
        }
    }
    r.lastUpdatedMs = millis();
    if (r.confidence < 255) r.confidence++;
}
//fw+ dv-etx
void NextHopRouter::invalidateRoute(uint32_t dest, float penalty)
{
    auto it = routes.find(dest);
    if (it == routes.end()) return;
    RouteEntry &r = it->second;
    r.aggregated_cost += penalty;
    if (r.confidence > 0) r.confidence--;
    r.lastUpdatedMs = millis();
}
//fw+
float NextHopRouter::getHysteresisThreshold() const
{
    float thr = 0.5f;
    if (moduleConfig.has_node_mod_admin && moduleConfig.node_mod_admin.hysteresis_cost_threshold_tenths) {
        thr = (float)moduleConfig.node_mod_admin.hysteresis_cost_threshold_tenths / 10.0f;
    }
    return thr;
}
//fw+
uint8_t NextHopRouter::getMinConfidenceToUse() const
{
    uint8_t minConf = 0;
    if (moduleConfig.has_node_mod_admin && moduleConfig.node_mod_admin.min_confidence_to_use) {
        minConf = moduleConfig.node_mod_admin.min_confidence_to_use;
    }
    return 0; //TODO temp. remove this
}
//fw+ dv-etx
float NextHopRouter::estimateEtxFromSnr(float snr) const
{
    if (snr >= 10.0f) return 1.2f;
    if (snr >= 5.0f) return 1.6f;
    if (snr >= 0.0f) return 2.2f;
    return 3.0f;
}

PendingPacket::PendingPacket(meshtastic_MeshPacket *p, uint8_t numRetransmissions)
{
    packet = p;
    this->numRetransmissions = numRetransmissions - 1; // We subtract one, because we assume the user just did the first send
}

/**
 * Send a packet
 */
ErrorCode NextHopRouter::send(meshtastic_MeshPacket *p)
{
    nexthop_counter++;
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    // fw+, for unicasts, try passive DV-ETX route first; else fallback to legacy next_hop/none
    if (!isBroadcast(p->to) && !isToUs(p)) {
        RouteEntry r;
        if (lookupRoute(p->to, r)) {
            p->next_hop = r.next_hop;
            LOG_INFO("DV-ETX next hop for %x -> %x (cost=%.2f, conf=%u)", p->to, p->next_hop, r.aggregated_cost, r.confidence);
        } else {
            p->next_hop = getNextHop(p->to, p->relay_node);
            LOG_DEBUG("Legacy next hop for %x -> %x", p->to, p->next_hop);
        }
    } else {
        p->next_hop = getNextHop(p->to, p->relay_node);
        LOG_DEBUG("Setting next hop for packet with dest %x to %x", p->to, p->next_hop);
    }

    // If it's from us, ReliableRouter already handles retransmissions if want_ack is set. If a next hop is set and hop limit is
    // not 0 or want_ack is set, start retransmissions
    if ((!isFromUs(p) || !p->want_ack) && p->next_hop != NO_NEXT_HOP_PREFERENCE && (p->hop_limit > 0 || p->want_ack)) {
        // Safety: ensure chosen next_hop corresponds to some direct neighbor; else fallback to flooding
        if (!isDirectNeighborLastByte(p->next_hop)) {
            LOG_INFO("DV-ETX next_hop 0x%x not a direct neighbor; fallback to flooding", p->next_hop);
            p->next_hop = NO_NEXT_HOP_PREFERENCE;
        }
        //fw+ use dedicated retransmission pool first to avoid starving main pool
        {
            auto cpy = retransPacketPool.allocCopy(*p);
            if (!cpy) {
                LOG_WARN("Retrans pool empty; trying main packetPool for relay copy");
                cpy = packetPool.allocCopy(*p);
            }
            if (!cpy) {
                LOG_WARN("No slot for relay copy; skipping retransmission schedule");
            } else {
                startRetransmission(cpy); // start retransmission for relayed packet
            }
        }
    }

    return Router::send(p);
}

bool NextHopRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    bool wasFallback = false;
    bool weWereNextHop = false;
    bool wasUpgraded = false;
    bool seenRecently = wasSeenRecently(p, true, &wasFallback, &weWereNextHop,
                                        &wasUpgraded); // Updates history; returns false when an upgrade is detected

    // Handle hop_limit upgrade scenario for rebroadcasters
    if (wasUpgraded && perhapsHandleUpgradedPacket(p)) {
        return true; // we handled it, so stop processing
    }

    if (seenRecently) {
        printPacket("Ignore dupe incoming msg", p);

        if (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA) {
            rxDupe++;
            stopRetransmission(p->from, p->id);
        }

        //fw+ notify BroadcastAssist about upstream duplicate drops (by sender+id) without including module headers
        extern void fwplus_ba_onOverheardFromId(uint32_t from, uint32_t id);
        fwplus_ba_onOverheardFromId(getFrom(p), p->id);

        // If it was a fallback to flooding, try to relay again
        if (wasFallback) {
            LOG_INFO("Fallback to flooding from relay_node=0x%x", p->relay_node);
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id)) {
                reprocessPacket(p);
                perhapsRebroadcast(p);
            }
        } else {
            bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
            // If repeated and not in Tx queue anymore, try relaying again, or if we are the destination, send the ACK again
            if (isRepeated) {
                if (!findInTxQueue(p->from, p->id)) {
                    reprocessPacket(p);
                    if (!perhapsRebroadcast(p) && isToUs(p) && p->want_ack) {
                        sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
                    }
                }
            } else if (!weWereNextHop) {
                perhapsCancelDupe(p); // If it's a dupe, cancel relay if we were not explicitly asked to relay
            }
        }
        return true;
    }

    bool r = Router::shouldFilterReceived(p);
    // Observe traceroute traffic for scheduler cooldowns
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
        uint32_t now = millis();
        lastHeardTracerouteMs = now;
        if (!isBroadcast(p->to)) perDestLastHeardRouteMs[p->to] = now;
    }
    return r;
}
//fw+ compute ring and apply HeardAssist policy
bool NextHopRouter::shouldRelayTextWithThrottle(const meshtastic_MeshPacket *p, uint32_t &outJitterMs) const
{
    outJitterMs = 0;
    uint32_t now = millis();
    if (!heardThrottle.isActive(now)) return true; // no policy → allow
    // compute ring from hop_start and hop_limit if available
    uint32_t ring = 0;
    if (p->hop_start != 0 && p->hop_limit <= p->hop_start) ring = (uint32_t)(p->hop_start - p->hop_limit);
    if (ring == 0) return true;
    if (heardThrottle.scope_min_ring && ring < heardThrottle.scope_min_ring) {
        // near area: strong suppression (only 1 elected ideally)
        // Fall through to election anyway with very low percent if configured 0
    }
    uint8_t pct = heardThrottle.percent_per_ring[ring];
    if (pct == 0) {
        // cluster policy fallback: near/mid/far groups
        if (ring <= 2) pct = 1;         // near (r1-2)
        else if (ring <= 4) pct = 2;    // mid (r3-4)
        else pct = 3;                   // far (r5-7)
    }
    // deterministic election: hash(packet_id, our_last_byte, ring) % 100 < pct
    uint32_t h = 1469598103u;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
    mix(p->id);
    mix(nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum()));
    mix(ring);
    bool elected = (h % 100) < pct;
    if (!elected) return false;
    outJitterMs = heardThrottle.jitter_base_ms + ring * heardThrottle.jitter_slope_ms + (random(33));
    return true;
}


void NextHopRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    NodeNum ourNodeNum = getNodeNum();
    uint8_t ourRelayID = nodeDB->getLastByteOfNodeNum(ourNodeNum);
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply) {
        // Update next-hop for the original transmitter of this successful transmission to the relay node, but ONLY if "from"
        // is not 0 (means implicit ACK) and original packet was also relayed by this node, or we sent it directly to the
        // destination
        if (p->from != 0) {
            meshtastic_NodeInfoLite *origTx = nodeDB->getMeshNode(p->from);
            if (origTx) {
                // Either relayer of ACK was also a relayer of the packet, or we were the *only* relayer and the ACK came
                // directly from the destination
                bool wasAlreadyRelayer = wasRelayer(p->relay_node, p->decoded.request_id, p->to);
                bool weWereSoleRelayer = false;
                bool weWereRelayer = wasRelayer(ourRelayID, p->decoded.request_id, p->to, &weWereSoleRelayer);
                if ((weWereRelayer && wasAlreadyRelayer) ||
                    (p->hop_start != 0 && p->hop_start == p->hop_limit && weWereSoleRelayer)) {
                    if (origTx->next_hop != p->relay_node) { // Not already set
                        LOG_INFO("Update next hop of 0x%x to 0x%x based on ACK/reply (was relayer %d we were sole %d)", p->from,
                                 p->relay_node, wasAlreadyRelayer, weWereSoleRelayer);
                        origTx->next_hop = p->relay_node;
                    }

                    // FW+: lightweight DV-ETX hint only if the relayer is a direct neighbor
                    if (isDirectNeighborLastByte(p->relay_node)) {
                        float hopCost = estimateEtxFromSnr(p->rx_snr) + 1.0f; // local hop + ahead estimate
                        learnRoute(origTx->num, p->relay_node, hopCost);
                    }
                }
            }
        }
        if (!isToUs(p)) {
            Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
            // stop retransmission for the original packet
            stopRetransmission(p->to, p->decoded.request_id); // for original packet, from = to and id = request_id
        }
    }

    //fw+ traceroute learning: if this is a traceroute/routing control with route info, learn next-hop hints
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        if (p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
            learnFromRouteDiscoveryPayload(p);
        } else if (p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
            learnFromRoutingPayload(p);
        }
    }

    perhapsRebroadcast(p);
    //fw+ delegate proactive scheduling to a helper to ease upstream merges
    maybeScheduleTraceroute(millis());

    // handle the packet as normal
    Router::sniffReceived(p, c);
}

//fw+
bool NextHopRouter::canProbeGlobal(uint32_t now) const
{
    if (!moduleConfig.has_node_mod_admin || !moduleConfig.node_mod_admin.proactive_traceroute_enabled) return false;
    uint32_t cooldownH = moduleConfig.node_mod_admin.traceroute_global_cooldown_hours ?: 12;
    uint32_t maxPerDay = moduleConfig.node_mod_admin.traceroute_max_per_day ?: 6;
    bool cooldownOk = (lastHeardTracerouteMs == 0 || (now - lastHeardTracerouteMs) >= cooldownH * 3600000UL) &&
                      (lastProbeGlobalMs == 0 || (now - lastProbeGlobalMs) >= cooldownH * 3600000UL);
    bool dayBudgetOk = (probesTodayCounter < maxPerDay);
    float util = airTime->channelUtilizationPercent();
    uint32_t utilThr = moduleConfig.node_mod_admin.traceroute_chanutil_threshold_percent ?: 15;
    bool utilOk = util < (float)utilThr;
    return cooldownOk && dayBudgetOk && utilOk;
}

//fw+ DV-ETX adaptation from S&F custody: reward successful path
void NextHopRouter::rewardRouteOnDelivered(PacketId originalId, NodeNum sourceNode, uint8_t viaHopLastByte, int8_t rxSnr)
{
    // Only adapt for unicasts sourced by sourceNode (DMs). We map delivered notice back to dest = 0 for now,
    // because we don't carry explicit destination here. Use source context and last-hop hint.
    (void)originalId; // currently unused in aggregation
    if (viaHopLastByte == NO_NEXT_HOP_PREFERENCE) return;

    // Heuristic: treat observed cost as good (local hop from viaHop + 0.5 ahead)
    float hopCost = estimateEtxFromSnr((float)rxSnr) + 0.5f;
    // Use learnRoute keyed by destination = sourceNode (we want better path TO sourceNode for replies)
    learnRoute(sourceNode, viaHopLastByte, hopCost);
}

//fw+ DV-ETX adaptation from S&F custody: penalize failing path
void NextHopRouter::penalizeRouteOnFailed(PacketId originalId, NodeNum sourceNode, uint8_t viaHopLastByte, uint32_t reasonCode)
{
    (void)originalId;
    (void)viaHopLastByte;
    // Map routing error to penalty magnitude
    float penalty = 0.8f; // base
    switch ((meshtastic_Routing_Error)reasonCode) {
    case meshtastic_Routing_Error_NO_CHANNEL:
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
        penalty = 1.5f; // strong penalty for terminal NAK
        break;
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
        penalty = 0.3f; // transient congestion
        break;
    default:
        penalty = 0.8f;
        break;
    }
    invalidateRoute(sourceNode, penalty);
}
//fw+
bool NextHopRouter::canProbeDest(uint32_t dest, uint32_t now) const
{
    auto it = perDestLastProbeMs.find(dest);
    uint32_t lastP = (it == perDestLastProbeMs.end()) ? 0 : it->second;
    uint32_t perH = moduleConfig.node_mod_admin.traceroute_per_dest_cooldown_hours ?: 12;
    if (lastP && (now - lastP) < perH * 3600000UL) return false;
    auto it2 = perDestLastHeardRouteMs.find(dest);
    uint32_t lastH = (it2 == perDestLastHeardRouteMs.end()) ? 0 : it2->second;
    uint32_t globH = moduleConfig.node_mod_admin.traceroute_global_cooldown_hours ?: 12;
    if (lastH && (now - lastH) < globH * 3600000UL) return false;
    return true;
}
//fw+
bool NextHopRouter::maybeScheduleTraceroute(uint32_t now)
{
    if (!moduleConfig.has_node_mod_admin || !moduleConfig.node_mod_admin.proactive_traceroute_enabled) return false;

    if (probesDayStartMs == 0 || now - probesDayStartMs > 24UL * 60UL * 60UL * 1000UL) {
        probesDayStartMs = now;
        probesTodayCounter = 0;
    }

    float staleRatio = computeStaleRatio(now);
    uint32_t thr = moduleConfig.node_mod_admin.traceroute_stale_ratio_threshold_percent ?: 30;
    if (staleRatio < (float)thr) return false;
    if (!canProbeGlobal(now)) return false;

    uint32_t chosenDest = 0;
    for (const auto &kv : routes) {
        const uint32_t dest = kv.first;
        const RouteEntry &r = kv.second;
        if (r.next_hop == NO_NEXT_HOP_PREFERENCE) continue;
        if (!isRouteStale(r, now)) continue;
        if (!canProbeDest(dest, now)) continue;
        chosenDest = dest;
        break;
    }
    if (!chosenDest) return false;

    if (sendTracerouteTo(chosenDest)) {
        lastProbeGlobalMs = now;
        perDestLastProbeMs[chosenDest] = now;
        probesTodayCounter++;
        return true;
    }
    return false;
}
//fw+
bool NextHopRouter::sendTracerouteTo(uint32_t dest)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) return false;
    p->to = dest;
    p->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
    p->decoded.want_response = true;
    meshtastic_RouteDiscovery req = meshtastic_RouteDiscovery_init_zero;
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_RouteDiscovery_msg, &req);

    uint32_t startHop = moduleConfig.node_mod_admin.traceroute_expanding_ring_initial_hop ?: 1;
    uint32_t maxHops = moduleConfig.node_mod_admin.traceroute_expanding_ring_max_hops ?: 3;
    meshtastic_NodeInfoLite *ninfo = nodeDB->getMeshNode(dest);
    if (ninfo && ninfo->hops_away)
        p->hop_limit = min((uint32_t)(ninfo->hops_away + 1), maxHops);
    else
        p->hop_limit = startHop;

    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    uint32_t jitter = moduleConfig.node_mod_admin.traceroute_probe_jitter_ms ?: 5000;
    if (jitter) p->tx_after = millis() + (random(jitter + 1));

    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_INFO("Scheduled proactive traceroute to 0x%x, hop_limit=%u", dest, p->hop_limit);
    return true;
}

//fw+ Check if we should be rebroadcasting this packet (with HeardAssist throttle and retrans pool)
bool NextHopRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
{
    //fw+
    if (!isToUs(p) && (p->hop_limit <= 0) && !isFromUs(p)) {
        blocked_by_hoplimit++;
    }

    if (!isToUs(p) && !isFromUs(p) && p->hop_limit > 0) {
        //fw+ Optional throttle for text broadcasts based on HeardAssist
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
            uint32_t jitter = 0;
            if (shouldRelayTextWithThrottle(p, jitter)) {
                // delay relay by jitter
                meshtastic_MeshPacket *tosend = retransPacketPool.allocCopy(*p);
                if (!tosend) tosend = packetPool.allocCopy(*p);
                if (tosend) {
                    tosend->hop_limit--;
                    if (jitter) tosend->tx_after = millis() + jitter;
                    NextHopRouter::send(tosend);
                } else {
                    LOG_WARN("Pool exhausted; skipping throttled relay");
                }
                return true;
            } else {
                return false; // suppressed by throttle
            }
        }
        // Check if packet has next_hop not set OR addressed to us
        if (p->id != 0) {
            if (isRebroadcaster()) {
                if (p->next_hop == NO_NEXT_HOP_PREFERENCE || p->next_hop == nodeDB->getLastByteOfNodeNum(getNodeNum())) {
                    //fw+ use retrans pool first; preserve hop_limit when policy dictates
                    meshtastic_MeshPacket *tosend = retransPacketPool.allocCopy(*p);
                    if (!tosend) tosend = packetPool.allocCopy(*p);
                    if (tosend) {
                        LOG_INFO("Rebroadcast received message coming from %x", p->relay_node);
                        
                        // Use shared logic to determine if hop_limit should be decremented
                        if (shouldDecrementHopLimit(p)) {
                            tosend->hop_limit--; // bump down the hop count
                        } else {
                            LOG_INFO("favorite-ROUTER/CLIENT_BASE-to-ROUTER/CLIENT_BASE rebroadcast: preserving hop_limit");
                        }
#if USERPREFS_EVENT_MODE
                        if (tosend->hop_limit > 2) {
                            // if we are "correcting" the hop_limit, "correct" the hop_start by the same amount to preserve hops away.
                            tosend->hop_start -= (tosend->hop_limit - 2);
                            tosend->hop_limit = 2;
                        }
#endif

                        if (p->next_hop == NO_NEXT_HOP_PREFERENCE) {
                            FloodingRouter::send(tosend);
                        } else {
                            NextHopRouter::send(tosend);
                        }
                        return true;
                    } else {
                        LOG_WARN("Pool exhausted; skipping relay");
                    }
                }
            } else {
                LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
            }
        } else {
            LOG_DEBUG("Ignore 0 id broadcast");
        }
    }

    return false;
}

/**
 * Get the next hop for a destination, given the relay node
 * @return the node number of the next hop, 0 if no preference (fallback to FloodingRouter)
 */
uint8_t NextHopRouter::getNextHop(NodeNum to, uint8_t relay_node)
{
    if (isBroadcast(to))
        return NO_NEXT_HOP_PREFERENCE;

    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(to);
    if (node && node->next_hop) {
        // We are careful not to return the relay node as the next hop
        if (node->next_hop != relay_node) {
            LOG_DEBUG("Next hop for 0x%x is 0x%x", to, node->next_hop);
            return node->next_hop;
        } else
            LOG_WARN("Next hop for 0x%x is 0x%x, same as relayer; set no pref", to, node->next_hop);
    }
    return NO_NEXT_HOP_PREFERENCE;
}

PendingPacket *NextHopRouter::findPendingPacket(GlobalPacketId key)
{
    auto old = pending.find(key); // If we have an old record, someone messed up because id got reused
    if (old != pending.end()) {
        return &old->second;
    } else
        return NULL;
}

/**
 * Stop any retransmissions we are doing of the specified node/packet ID pair
 */
bool NextHopRouter::stopRetransmission(NodeNum from, PacketId id)
{
    auto key = GlobalPacketId(from, id);
    return stopRetransmission(key);
}

bool NextHopRouter::roleAllowsCancelingFromTxQueue(const meshtastic_MeshPacket *p)
{
    // Return true if we're allowed to cancel a packet in the txQueue (so we may never transmit it even once)

    // Return false for roles like ROUTER, ROUTER_LATE which should always transmit the packet at least once.

    return roleAllowsCancelingDupe(p); // same logic as FloodingRouter::roleAllowsCancelingDupe
}

bool NextHopRouter::stopRetransmission(GlobalPacketId key)
{
    auto old = findPendingPacket(key);
    if (old) {
        auto p = old->packet;
        /* Only when we already transmitted a packet via LoRa, we will cancel the packet in the Tx queue
          to avoid canceling a transmission if it was ACKed super fast via MQTT */
        if (old->numRetransmissions < NUM_RELIABLE_RETX - 1) {
            // We only cancel it if we are the original sender or if we're not a router(_late)
            if (isFromUs(p) || roleAllowsCancelingFromTxQueue(p)) {
                // remove the 'original' (identified by originator and packet->id) from the txqueue and free it
                cancelSending(getFrom(p), p->id);
            }
        }

        // Regardless of whether or not we canceled this packet from the txQueue, remove it from our pending list so it
        // doesn't get scheduled again. (This is the core of stopRetransmission.)
        auto numErased = pending.erase(key);
        assert(numErased == 1);

        // When we remove an entry from pending, always be sure to release the copy of the packet that was allocated in the
        // call to startRetransmission.
        packetPool.release(p);

        return true;
    } else
        return false;
}

/**
 * Add p to the list of packets to retransmit occasionally.  We will free it once we stop retransmitting.
 */
PendingPacket *NextHopRouter::startRetransmission(meshtastic_MeshPacket *p, uint8_t numReTx)
{
    auto id = GlobalPacketId(p);
    auto rec = PendingPacket(p, numReTx);

    stopRetransmission(getFrom(p), p->id);

    setNextTx(&rec);
    pending[id] = rec;

    return &pending[id];
}

/**
 * Do any retransmissions that are scheduled (FIXME - for the time being called from loop)
 */
int32_t NextHopRouter::doRetransmissions()
{
    uint32_t now = millis();
    int32_t d = INT32_MAX;

    // FIXME, we should use a better datastructure rather than walking through this map.
    // for(auto el: pending) {
    for (auto it = pending.begin(), nextIt = it; it != pending.end(); it = nextIt) {
        ++nextIt; // we use this odd pattern because we might be deleting it...
        auto &p = it->second;

        bool stillValid = true; // assume we'll keep this record around

        // FIXME, handle 51 day rolloever here!!!
        if (p.nextTxMsec <= now) {
            if (p.numRetransmissions == 0) {
                if (isFromUs(p.packet)) {
                    LOG_DEBUG("Reliable send failed, returning a nak for fr=0x%x,to=0x%x,id=0x%x", p.packet->from, p.packet->to,
                              p.packet->id);
                    sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, getFrom(p.packet), p.packet->id, p.packet->channel);
                }
                // Note: we don't stop retransmission here, instead the Nak packet gets processed in sniffReceived
                stopRetransmission(it->first);
                stillValid = false; // just deleted it
            } else {
                LOG_DEBUG("Sending retransmission fr=0x%x,to=0x%x,id=0x%x, tries left=%d", p.packet->from, p.packet->to,
                          p.packet->id, p.numRetransmissions);

                if (!isBroadcast(p.packet->to)) {
                    if (p.numRetransmissions == 1) {
                        // Last retransmission: try backup next-hop; if none, fallback to FloodingRouter
                        RouteEntry rSnapshot;
                        bool usedBackup = false;
                        if (lookupRoute(p.packet->to, rSnapshot) && rSnapshot.backup_next_hop != NO_NEXT_HOP_PREFERENCE) {
                            meshtastic_MeshPacket *tryBackup = retransPacketPool.allocCopy(*p.packet);
                            if (!tryBackup) tryBackup = packetPool.allocCopy(*p.packet);
                            if (tryBackup) {
                                tryBackup->next_hop = rSnapshot.backup_next_hop;
                                LOG_INFO("Retry final with backup next hop for dest 0x%x -> 0x%x", p.packet->to, tryBackup->next_hop);
                                NextHopRouter::send(tryBackup);
                                usedBackup = true;
                            } else {
                                LOG_WARN("Pool exhausted for backup retry; skipping backup path");
                            }
                        }
                        if (!usedBackup) {
                            p.packet->next_hop = NO_NEXT_HOP_PREFERENCE;
                            // Also reset it in the nodeDB
                            meshtastic_NodeInfoLite *sentTo = nodeDB->getMeshNode(p.packet->to);
                            if (sentTo) {
                                LOG_INFO("Resetting next hop for packet with dest 0x%x\n", p.packet->to);
                                sentTo->next_hop = NO_NEXT_HOP_PREFERENCE;
                            }
                            {
                                auto c = retransPacketPool.allocCopy(*p.packet);
                                if (!c) c = packetPool.allocCopy(*p.packet);
                                if (c) FloodingRouter::send(c);
                                else LOG_WARN("Pool exhausted for flooding retry; skip");
                            }
                        }
                    } else {
                        {
                            auto c = retransPacketPool.allocCopy(*p.packet);
                            if (!c) c = packetPool.allocCopy(*p.packet);
                            if (c) NextHopRouter::send(c);
                            else LOG_WARN("Pool exhausted for next-hop retry; skip");
                        }
                    }
                } else {
                    // Note: we call the superclass version because we don't want to have our version of send() add a new
                    // retransmission record
                    {
                        auto c = retransPacketPool.allocCopy(*p.packet);
                        if (!c) c = packetPool.allocCopy(*p.packet);
                        if (c) FloodingRouter::send(c);
                        else LOG_WARN("Pool exhausted for flooding retry; skip");
                    }
                }

                // Queue again
                --p.numRetransmissions;
                setNextTx(&p);
            }
        }

        if (stillValid) {
            // Update our desired sleep delay
            int32_t t = p.nextTxMsec - now;

            d = min(t, d);
        }
    }

    return d;
}

void NextHopRouter::setNextTx(PendingPacket *pending)
{
    assert(iface);
    auto d = iface->getRetransmissionMsec(pending->packet);
    pending->nextTxMsec = millis() + d;
    LOG_DEBUG("Setting next retransmission in %u msecs: ", d);
    printPacket("", pending->packet);
    setReceivedMessage(); // Run ASAP, so we can figure out our correct sleep time
}

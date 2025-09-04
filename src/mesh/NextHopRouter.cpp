#include "NextHopRouter.h"
#include "MeshService.h"
#include <pb_encode.h>

NextHopRouter::NextHopRouter() {}
//fw+ dv-etx
bool NextHopRouter::lookupRoute(uint32_t dest, RouteEntry &out)
{
    auto it = routes.find(dest);
    if (it == routes.end()) return false;
    const RouteEntry &r = it->second;
    // TTL (adaptive) and minimal confidence gate
    if (millis() - r.lastUpdatedMs > computeRouteTtlMs(r.confidence)) return false;
    if (r.confidence < 2) return false;
    out = r;
    return (out.next_hop != NO_NEXT_HOP_PREFERENCE);
}
//fw+ dv-etx
void NextHopRouter::learnRoute(uint32_t dest, uint8_t viaHop, float observedCost)
{
    if (viaHop == NO_NEXT_HOP_PREFERENCE) return;
    RouteEntry &r = routes[dest];
    if (r.confidence == 0) {
        r.aggregated_cost = observedCost;
    } else {
        // Smooth update
        r.aggregated_cost = 0.7f * r.aggregated_cost + 0.3f * observedCost;
    }
    // Update backup if the new candidate is different and better than current
    if (r.next_hop != viaHop) {
        if (r.next_hop == NO_NEXT_HOP_PREFERENCE) {
            r.next_hop = viaHop;
        } else {
            // Decide which is primary/backup by cost
            if (observedCost < r.aggregated_cost) {
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
    if ((!isFromUs(p) || !p->want_ack) && p->next_hop != NO_NEXT_HOP_PREFERENCE && (p->hop_limit > 0 || p->want_ack))
        startRetransmission(packetPool.allocCopy(*p)); // start retransmission for relayed packet

    return Router::send(p);
}

bool NextHopRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    bool wasFallback = false;
    bool weWereNextHop = false;
    if (wasSeenRecently(p, true, &wasFallback, &weWereNextHop)) { // Note: this will also add a recent packet record
        printPacket("Ignore dupe incoming msg", p);
        rxDupe++;
        stopRetransmission(p->from, p->id);

        // If it was a fallback to flooding, try to relay again
        if (wasFallback) {
            LOG_INFO("Fallback to flooding from relay_node=0x%x", p->relay_node);
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id))
                perhapsRelay(p);
        } else {
            bool isRepeated = p->hop_start > 0 && p->hop_start == p->hop_limit;
            // If repeated and not in Tx queue anymore, try relaying again, or if we are the destination, send the ACK again
            if (isRepeated) {
                if (!findInTxQueue(p->from, p->id) && !perhapsRelay(p) && isToUs(p) && p->want_ack)
                    sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
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

void NextHopRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    NodeNum ourNodeNum = getNodeNum();
    uint8_t ourRelayID = nodeDB->getLastByteOfNodeNum(ourNodeNum);
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply) {
        // Update next-hop for the original transmitter of this successful transmission to the relay node, but ONLY if "from" is
        // not 0 (means implicit ACK) and original packet was also relayed by this node, or we sent it directly to the destination
        if (p->from != 0) {
            meshtastic_NodeInfoLite *origTx = nodeDB->getMeshNode(p->from);
            if (origTx) {
                // Either relayer of ACK was also a relayer of the packet, or we were the relayer and the ACK came directly from
                // the destination
                //fw+ dv-etx mod
                bool weRelayedOriginal = wasRelayer(ourRelayID, p->decoded.request_id, p->to);
                bool ackRelayerWasOnPath = wasRelayer(p->relay_node, p->decoded.request_id, p->to);
                if (ackRelayerWasOnPath || (weRelayedOriginal && p->hop_start != 0 && p->hop_start == p->hop_limit)) {
                    // Passive learn: dest is p->from (original sender) or p->to depending on context; for DM replies, p->to tends do wskazuje na nas
                    uint32_t destNode = origTx->num; // original transmitter node id (peer)
                    float hopCost = estimateEtxFromSnr(p->rx_snr) + 1.0f; // local hop + ahead estimate
                    learnRoute(destNode, p->relay_node, hopCost);
                    origTx->next_hop = p->relay_node; // keep legacy hint too
                }
            }
        }
        if (!isToUs(p)) {
            Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
            // stop retransmission for the original packet
            stopRetransmission(p->to, p->decoded.request_id); // for original packet, from = to and id = request_id
        }
    }

    // fw+ traceroute learning: if this is a traceroute/routing control with route info, learn next-hop hints
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        // Two possible containers: direct TRACEROUTE_APP in Data.payload as Routing with route_(request|reply),
        // or ROUTING_APP containing Routing. Prefer decoding when portnum matches and payload not empty.
        if (p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP || p->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
            meshtastic_Routing routing = meshtastic_Routing_init_default;
            if (pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_Routing_msg, &routing)) {
                const meshtastic_RouteDiscovery *rd = NULL;
                if (routing.which_variant == meshtastic_Routing_route_reply_tag) rd = &routing.route_reply;
                else if (routing.which_variant == meshtastic_Routing_route_request_tag) rd = &routing.route_request;

                if (rd) {
                    // Determine which path list to use: prefer route_back if present (reply path), otherwise route
                    const pb_size_t maxHops = rd->route_back_count ? rd->route_back_count : rd->route_count;
                    const uint32_t *path = rd->route_back_count ? rd->route_back : rd->route;
                    const pb_size_t maxSnr = rd->snr_back_count ? rd->snr_back_count : rd->snr_towards_count;
                    const int8_t *snrList = rd->snr_back_count ? rd->snr_back : rd->snr_towards;

                    if (path && maxHops >= 2) {
                        uint32_t self = getNodeNum();
                        // find our index in the path
                        int selfIdx = -1;
                        for (pb_size_t i = 0; i < maxHops; ++i) {
                            if (path[i] == self) { selfIdx = (int)i; break; }
                        }
                        if (selfIdx >= 0 && (selfIdx + 1) < (int)maxHops) {
                            uint32_t dest = path[maxHops - 1];
                            uint8_t nextHop = (uint8_t)(path[selfIdx + 1] & 0xFF);
                            // derive per-link ETX from SNR if available for this segment
                            float linkEtx = 2.0f;
                            if (snrList && maxSnr > (pb_size_t)selfIdx) {
                                // snr is in dB scaled by 4
                                float snr = (float)snrList[selfIdx] / 4.0f;
                                linkEtx = estimateEtxFromSnr(snr);
                            } else {
                                // fall back to rx_snr observed on this packet (rough hint)
                                linkEtx = estimateEtxFromSnr(p->rx_snr);
                            }
                            // Add a simple ahead cost estimate: remaining hops count
                            int remainingHops = (int)maxHops - selfIdx - 1;
                            float observedCost = linkEtx + (remainingHops > 1 ? (remainingHops - 1) * 1.0f : 0.0f);
                            learnRoute(dest, nextHop, observedCost);
                        }
                    }
                }
            }
        }
    }

    perhapsRelay(p);
    //fw+ delegate proactive scheduling to a helper to ease upstream merges
    maybeScheduleTraceroute(millis());

    // handle the packet as normal
    Router::sniffReceived(p, c);
}

//fw+
bool NextHopRouter::canProbeGlobal(uint32_t now) const
{
    if (!moduleConfig.has_nodemodadmin || !moduleConfig.nodemodadmin.proactive_traceroute_enabled) return false;
    uint32_t cooldownH = moduleConfig.nodemodadmin.traceroute_global_cooldown_hours ?: 12;
    uint32_t maxPerDay = moduleConfig.nodemodadmin.traceroute_max_per_day ?: 6;
    bool cooldownOk = (lastHeardTracerouteMs == 0 || (now - lastHeardTracerouteMs) >= cooldownH * 3600000UL) &&
                      (lastProbeGlobalMs == 0 || (now - lastProbeGlobalMs) >= cooldownH * 3600000UL);
    bool dayBudgetOk = (probesTodayCounter < maxPerDay);
    float util = airTime->channelUtilizationPercent();
    uint32_t utilThr = moduleConfig.nodemodadmin.traceroute_chanutil_threshold_percent ?: 15;
    bool utilOk = util < (float)utilThr;
    return cooldownOk && dayBudgetOk && utilOk;
}
//fw+
bool NextHopRouter::canProbeDest(uint32_t dest, uint32_t now) const
{
    auto it = perDestLastProbeMs.find(dest);
    uint32_t lastP = (it == perDestLastProbeMs.end()) ? 0 : it->second;
    uint32_t perH = moduleConfig.nodemodadmin.traceroute_per_dest_cooldown_hours ?: 12;
    if (lastP && (now - lastP) < perH * 3600000UL) return false;
    auto it2 = perDestLastHeardRouteMs.find(dest);
    uint32_t lastH = (it2 == perDestLastHeardRouteMs.end()) ? 0 : it2->second;
    uint32_t globH = moduleConfig.nodemodadmin.traceroute_global_cooldown_hours ?: 12;
    if (lastH && (now - lastH) < globH * 3600000UL) return false;
    return true;
}
//fw+
bool NextHopRouter::maybeScheduleTraceroute(uint32_t now)
{
    if (!moduleConfig.has_nodemodadmin || !moduleConfig.nodemodadmin.proactive_traceroute_enabled) return false;

    if (probesDayStartMs == 0 || now - probesDayStartMs > 24UL * 60UL * 60UL * 1000UL) {
        probesDayStartMs = now;
        probesTodayCounter = 0;
    }

    float staleRatio = computeStaleRatio(now);
    uint32_t thr = moduleConfig.nodemodadmin.traceroute_stale_ratio_threshold_percent ?: 30;
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

    uint32_t startHop = moduleConfig.nodemodadmin.traceroute_expanding_ring_initial_hop ?: 1;
    uint32_t maxHops = moduleConfig.nodemodadmin.traceroute_expanding_ring_max_hops ?: 3;
    meshtastic_NodeInfoLite *ninfo = nodeDB->getMeshNode(dest);
    if (ninfo && ninfo->hops_away)
        p->hop_limit = min((uint32_t)(ninfo->hops_away + 1), maxHops);
    else
        p->hop_limit = startHop;

    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    uint32_t jitter = moduleConfig.nodemodadmin.traceroute_probe_jitter_ms ?: 5000;
    if (jitter) p->tx_after = millis() + (random(jitter + 1));

    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_INFO("Scheduled proactive traceroute to 0x%x, hop_limit=%u", dest, p->hop_limit);
    return true;
}

/* Check if we should be relaying this packet if so, do so. */
bool NextHopRouter::perhapsRelay(const meshtastic_MeshPacket *p)
{
    //fw+
    if (!isToUs(p) && (p->hop_limit <= 0) && !isFromUs(p)) {
        blocked_by_hoplimit++;
    }

    if (!isToUs(p) && !isFromUs(p) && p->hop_limit > 0) {
        if (p->next_hop == NO_NEXT_HOP_PREFERENCE || p->next_hop == nodeDB->getLastByteOfNodeNum(getNodeNum())) {
            if (isRebroadcaster()) {
                meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p); // keep a copy because we will be sending it
                LOG_INFO("Relaying received message coming from %x", p->relay_node);

                tosend->hop_limit--; // bump down the hop count
                NextHopRouter::send(tosend);

                return true;
            } else {
                LOG_DEBUG("Not rebroadcasting: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
            }
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
    // When we're a repeater router->sniffReceived will call NextHopRouter directly without checking for broadcast
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

bool NextHopRouter::stopRetransmission(GlobalPacketId key)
{
    auto old = findPendingPacket(key);
    if (old) {
        auto p = old->packet;
        /* Only when we already transmitted a packet via LoRa, we will cancel the packet in the Tx queue
          to avoid canceling a transmission if it was ACKed super fast via MQTT */
        if (old->numRetransmissions < NUM_RELIABLE_RETX - 1) {
            // We only cancel it if we are the original sender or if we're not a router(_late)/repeater
            if (isFromUs(p) || (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER &&
                                config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER &&
                                config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)) {
                // remove the 'original' (identified by originator and packet->id) from the txqueue and free it
                cancelSending(getFrom(p), p->id);
                // now free the pooled copy for retransmission too
                packetPool.release(p);
            }
        }
        auto numErased = pending.erase(key);
        assert(numErased == 1);
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
                            meshtastic_MeshPacket *tryBackup = packetPool.allocCopy(*p.packet);
                            tryBackup->next_hop = rSnapshot.backup_next_hop;
                            LOG_INFO("Retry final with backup next hop for dest 0x%x -> 0x%x", p.packet->to, tryBackup->next_hop);
                            NextHopRouter::send(tryBackup);
                            usedBackup = true;
                        }
                        if (!usedBackup) {
                            p.packet->next_hop = NO_NEXT_HOP_PREFERENCE;
                            // Also reset it in the nodeDB
                            meshtastic_NodeInfoLite *sentTo = nodeDB->getMeshNode(p.packet->to);
                            if (sentTo) {
                                LOG_INFO("Resetting next hop for packet with dest 0x%x\n", p.packet->to);
                                sentTo->next_hop = NO_NEXT_HOP_PREFERENCE;
                            }
                            FloodingRouter::send(packetPool.allocCopy(*p.packet));
                        }
                    } else {
                        NextHopRouter::send(packetPool.allocCopy(*p.packet));
                    }
                } else {
                    // Note: we call the superclass version because we don't want to have our version of send() add a new
                    // retransmission record
                    FloodingRouter::send(packetPool.allocCopy(*p.packet));
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
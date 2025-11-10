#pragma once

#include "FloodingRouter.h"
#include <unordered_map>
#include "mesh/generated/meshtastic/heard.pb.h" //fw+

/**
 * An identifier for a globally unique message - a pair of the sending nodenum and the packet id assigned
 * to that message
 */
struct GlobalPacketId {
    NodeNum node;
    PacketId id;

    bool operator==(const GlobalPacketId &p) const { return node == p.node && id == p.id; }

    explicit GlobalPacketId(const meshtastic_MeshPacket *p)
    {
        node = getFrom(p);
        id = p->id;
    }

    GlobalPacketId(NodeNum _from, PacketId _id)
    {
        node = _from;
        id = _id;
    }
};

/**
 * A packet queued for retransmission
 */
struct PendingPacket {
    meshtastic_MeshPacket *packet;

    /** The next time we should try to retransmit this packet */
    uint32_t nextTxMsec = 0;

    /** Starts at NUM_RETRANSMISSIONS -1 and counts down.  Once zero it will be removed from the list */
    uint8_t numRetransmissions = 0;

    PendingPacket() {}
    explicit PendingPacket(meshtastic_MeshPacket *p, uint8_t numRetransmissions);
};

class GlobalPacketIdHashFunction
{
  public:
    size_t operator()(const GlobalPacketId &p) const { return (std::hash<NodeNum>()(p.node)) ^ (std::hash<PacketId>()(p.id)); }
};

/*
  Router for direct messages, which only relays if it is the next hop for a packet. The next hop is set by the current
  relayer of a packet, which bases this on information from a previous successful delivery to the destination via flooding.
  Namely, in the PacketHistory, we keep track of (up to 3) relayers of a packet. When the ACK is delivered back to us via a node
  that also relayed the original packet, we use that node as next hop for the destination from then on. This makes sure that only
  when there’s a two-way connection, we assign a next hop. Both the ReliableRouter and NextHopRouter will do retransmissions (the
  NextHopRouter only 1 time). For the final retry, if no one actually relayed the packet, it will reset the next hop in order to
  fall back to the FloodingRouter again. Note that thus also intermediate hops will do a single retransmission if the intended
  next-hop didn’t relay, in order to fix changes in the middle of the route.
*/
class NextHopRouter : public FloodingRouter
{
  public:
    /**
     * Constructor
     *
     */
    NextHopRouter();

    NextHopRouter *asNextHopRouter() override { return this; }
    const NextHopRouter *asNextHopRouter() const override { return this; }

    /**
     * Send a packet
     * @return an error code
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;

    /** Do our retransmission handling */
    virtual int32_t runOnce() override
    {
        // Note: We must doRetransmissions FIRST, because it might queue up work for the base class runOnce implementation
        doRetransmissions();

        int32_t r = FloodingRouter::runOnce();

        // Also after calling runOnce there might be new packets to retransmit
        auto d = doRetransmissions();
        return min(d, r);
    }

    // The number of retransmissions intermediate nodes will do (actually 1 less than this)
    constexpr static uint8_t NUM_INTERMEDIATE_RETX = 2;
    // The number of retransmissions the original sender will do
    constexpr static uint8_t NUM_RELIABLE_RETX = 3;

    //fw+ Adapt DV-ETX using S&F custody signals (override Router's no-op)
    void rewardRouteOnDelivered(PacketId originalId, NodeNum sourceNode, uint8_t viaHopLastByte, int8_t rxSnr) override;
    void penalizeRouteOnFailed(PacketId originalId, NodeNum sourceNode, uint8_t viaHopLastByte, uint32_t reasonCode) override;

    //fw+ PUBLIC API for route learning from traceroute packets (called by TraceRouteModule)
    // This enables ACTIVE and PASSIVE DV-ETX learning from traceroute responses
    void learnFromRouteDiscoveryPayload(const meshtastic_MeshPacket *p);
    
    //fw+ PUBLIC API for route learning from DTN custody chains (called by DtnOverlayModule)
    // This enables PASSIVE DV-ETX learning from observed DTN custody paths
    void learnFromDtnCustodyPath(const uint32_t *path, size_t pathLen);

  protected:
    /**
     * Pending retransmissions
     */
    std::unordered_map<GlobalPacketId, PendingPacket, GlobalPacketIdHashFunction> pending;

    //fw+ dv-etx, passive DV-ETX (lightweight, optional)
    struct RouteEntry {
        uint8_t next_hop = NO_NEXT_HOP_PREFERENCE; // last-byte id
        float aggregated_cost = 0.0f;               // simple scalar cost
        uint8_t confidence = 0;                    // how sure we are (seen successes)
        uint32_t lastUpdatedMs = 0;
        uint8_t backup_next_hop = NO_NEXT_HOP_PREFERENCE; // secondary candidate
        float backup_cost = 0.0f;
    };

    std::unordered_map<uint32_t, RouteEntry> routes; // dest_node_id -> route

    //fw+ proactive traceroute scheduler state
    uint32_t lastHeardTracerouteMs = 0;
    uint32_t lastProbeGlobalMs = 0;
    uint32_t probesTodayCounter = 0;
    uint32_t probesDayStartMs = 0;
    std::unordered_map<uint32_t, uint32_t> perDestLastHeardRouteMs; // dest -> last heard traceroute time
    std::unordered_map<uint32_t, uint32_t> perDestLastProbeMs;      // dest -> last probe time

    bool isRouteStale(const RouteEntry &r, uint32_t now) const
    {
        uint32_t ttlMs = computeRouteTtlMs(r.confidence);
        // Treat as stale if age exceeds 75% of TTL
        return (now - r.lastUpdatedMs) > (ttlMs - ttlMs / 4);
    }

    float computeStaleRatio(uint32_t now) const
    {
        size_t total = 0, stale = 0;
        for (const auto &kv : routes) {
            const RouteEntry &r = kv.second;
            if (r.next_hop == NO_NEXT_HOP_PREFERENCE) continue;
            total++;
            if (isRouteStale(r, now)) stale++;
        }
        if (total == 0) return 0.0f;
        return (float)stale * 100.0f / (float)total;
    }

    bool canProbeGlobal(uint32_t now) const;
    bool canProbeDest(uint32_t dest, uint32_t now) const;
    bool maybeScheduleTraceroute(uint32_t now);
    bool sendTracerouteTo(uint32_t dest);
    // Config-driven thresholds
    float getHysteresisThreshold() const;
    uint8_t getMinConfidenceToUse() const;
    //fw+ dv-etx
    bool lookupRoute(uint32_t dest, RouteEntry &out);
    void learnRoute(uint32_t dest, uint8_t viaHop, float observedCost);
    void invalidateRoute(uint32_t dest, float penalty = 1.0f);
    float estimateEtxFromSnr(float snr) const;
    // fw+ nexthop snif (learnFromRouteDiscoveryPayload now in public section above)
    void learnFromRoutingPayload(const meshtastic_MeshPacket *p);
    void processPathAndLearn(const uint32_t *path, size_t maxHops,
                             const int8_t *snrList, size_t maxSnr,
                             const meshtastic_MeshPacket *p);
    bool isDirectNeighborLastByte(uint8_t lastByte) const
    {
        // Scan known nodes for any with matching last byte that are direct (hops_away == 0)
        int totalNodes = nodeDB->getNumMeshNodes();
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
            if (!n) continue;
            if (n->hops_away == 0 && (uint8_t)(n->num & 0xFF) == lastByte) return true;
        }
        return false;
    }
    
    //fw+ HeardAssist throttle state
    struct HeardThrottlePolicy {
        uint8_t percent_per_ring[8] = {0}; // index 1..7 used
        uint32_t jitter_base_ms = 0;
        uint32_t jitter_slope_ms = 0;
        uint8_t scope_min_ring = 0;
        uint32_t active_until_ms = 0;
        bool isActive(uint32_t now) const { return now < active_until_ms; }
    };
    HeardThrottlePolicy heardThrottle;

    bool shouldRelayTextWithThrottle(const meshtastic_MeshPacket *p, uint32_t &outJitterMs) const; //fw+


    /**
     * Should this incoming filter be dropped?
     *
     * Called immediately on reception, before any further processing.
     * @return true to abandon the packet
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) override;

    /**
     * Look for packets we need to relay
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c) override;

    /**
     * Try to find the pending packet record for this ID (or NULL if not found)
     */
    PendingPacket *findPendingPacket(NodeNum from, PacketId id) { return findPendingPacket(GlobalPacketId(from, id)); }
    PendingPacket *findPendingPacket(GlobalPacketId p);

    /**
     * Add p to the list of packets to retransmit occasionally.  We will free it once we stop retransmitting.
     */
    PendingPacket *startRetransmission(meshtastic_MeshPacket *p, uint8_t numReTx = NUM_INTERMEDIATE_RETX);

    // Return true if we're allowed to cancel a packet in the txQueue (so we may never transmit it even once)
    bool roleAllowsCancelingFromTxQueue(const meshtastic_MeshPacket *p);

    /**
     * Stop any retransmissions we are doing of the specified node/packet ID pair
     *
     * @return true if we found and removed a transmission with this ID
     */
    bool stopRetransmission(NodeNum from, PacketId id);
    bool stopRetransmission(GlobalPacketId p);

    /**
     * Do any retransmissions that are scheduled (FIXME - for the time being called from loop)
     *
     * @return the number of msecs until our next retransmission or MAXINT if none scheduled
     */
    int32_t doRetransmissions();

    void setNextTx(PendingPacket *pending);

  private:
    /**
     * Get the next hop for a destination, given the relay node
     * @return the node number of the next hop, 0 if no preference (fallback to FloodingRouter)
     */
    uint8_t getNextHop(NodeNum to, uint8_t relay_node);

    /** Check if we should be rebroadcasting this packet if so, do so.
     *  @return true if we did rebroadcast */
    bool perhapsRebroadcast(const meshtastic_MeshPacket *p) override;

  public:
    //fw+ Public API for route introspection
    struct PublicRouteEntry {
        uint32_t dest;
        uint8_t next_hop;
        float aggregated_cost;
        uint8_t confidence;
        uint32_t lastUpdatedMs;
    };

    //fw+ Adaptive TTL: base + per-confidence increment, clamped to max
    constexpr static uint32_t ROUTE_TTL_BASE_MS = 24UL * 60UL * 60UL * 1000UL;   // 24 hours
    constexpr static uint32_t ROUTE_TTL_PER_CONF_MS = 12UL * 60UL * 60UL * 1000UL; // +12 hours per confidence
    constexpr static uint32_t ROUTE_TTL_MAX_MS = 7UL * 24UL * 60UL * 60UL * 1000UL; // cap at 7 days

    static uint32_t computeRouteTtlMs(uint8_t confidence)
    {
        // Allow admin overrides if set (>0)
        uint32_t baseMs = ROUTE_TTL_BASE_MS;
        uint32_t perConfMs = ROUTE_TTL_PER_CONF_MS;
        uint32_t maxMs = ROUTE_TTL_MAX_MS;
        if (moduleConfig.has_node_mod_admin) {
            if (moduleConfig.node_mod_admin.route_ttl_base_hours) baseMs = moduleConfig.node_mod_admin.route_ttl_base_hours * 3600000UL;
            if (moduleConfig.node_mod_admin.route_ttl_per_conf_hours) perConfMs = moduleConfig.node_mod_admin.route_ttl_per_conf_hours * 3600000UL;
            if (moduleConfig.node_mod_admin.route_ttl_max_hours) maxMs = moduleConfig.node_mod_admin.route_ttl_max_hours * 3600000UL;
        }
        uint64_t ttl = baseMs + (uint64_t)confidence * perConfMs;
        if (ttl > maxMs) ttl = maxMs;
        return (uint32_t)ttl;
    }

    std::vector<PublicRouteEntry> getRouteSnapshot(bool includeStale = false) const
    {
        std::vector<PublicRouteEntry> out;
        out.reserve(routes.size());
        uint32_t now = millis();
        for (const auto &kv : routes) {
            const uint32_t dest = kv.first;
            const RouteEntry &r = kv.second;
            if (!includeStale) {
                uint32_t ttlMs = computeRouteTtlMs(r.confidence);
                if (now - r.lastUpdatedMs > ttlMs) continue;
                // Visibility threshold aligned with routing usage threshold
                if (r.confidence < getMinConfidenceToUse()) continue;
            }
            PublicRouteEntry e{dest, r.next_hop, r.aggregated_cost, r.confidence, r.lastUpdatedMs};
            out.push_back(e);
        }
        return out;
    }

    //fw+ expose confidence check to callers without RTTI
    bool hasRouteConfidence(NodeNum dest, uint8_t minConf) const override
    {
        auto snap = getRouteSnapshot(true);
        for (const auto &e : snap) if (e.dest == dest) return e.confidence >= minConf;
        return false;
    }
};
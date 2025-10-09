#pragma once

//fw+ DTN overlay module (FW+ only). Guard compile until protobufs are generated.
#if __has_include("mesh/generated/meshtastic/fwplus_dtn.pb.h")

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/fwplus_dtn.pb.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <unordered_map>

class DtnOverlayModule : private concurrency::OSThread, public ProtobufModule<meshtastic_FwplusDtn>
{
  public:
    // Construct DTN overlay module
    DtnOverlayModule();
    // Apply latest ModuleConfig.dtn_overlay at runtime (hot-reload)
    void reloadFromModuleConfig();
    // Query helpers for other subsystems
    bool isEnabled() const { return configEnabled; } //fw+
    uint32_t getTtlMinutes() const { return configTtlMinutes; } //fw+
    // Enqueue overlay data created from a captured DM (plaintext or encrypted)
    // deadlineMs uses uint64_t to avoid overflow (epoch*1000 exceeds uint32 in 2025+)
    void enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                             uint64_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                             bool allowProxyFallback);
    // Expose DTN stats snapshot for diagnostics
    size_t getPendingCount() const { return pendingById.size(); }
    struct DtnStatsSnapshot {
        size_t pendingCount;
        uint32_t forwardsAttempted;
        uint32_t fallbacksAttempted;
        uint32_t receiptsEmitted;
        uint32_t receiptsReceived;
        uint32_t expired;
        uint32_t giveUps;
        uint32_t milestonesSent;
        uint32_t probesSent;
        uint32_t fwplusUnresponsiveFallbacks; //fw+ count of fallbacks to unresponsive FW+ nodes
        uint32_t lastForwardAgeSecs;
        uint32_t knownNodesCount; //fw+ number of known FW+ nodes
        bool enabled;
    };
    // Fill snapshot with current counters
    void getStatsSnapshot(DtnStatsSnapshot &out) const;
    // Record observed FW+ version for a node (from receipt.reason)
    void recordFwplusVersion(NodeNum n, uint16_t version);
    // Periodically advertise FW+ version (one-shot early and periodic thereafter)
    void maybeAdvertiseFwplusVersion();

    // Check if DTN should intercept local DMs (intelligent routing) - PUBLIC for Router access
    bool shouldInterceptLocalDM() const {
        if (!configEnabled) return false;
        
        // If we don't know any DTN nodes, don't intercept - use native DM immediately
        // This prevents cold start delays and allows immediate delivery
        if (fwplusVersionByNode.empty()) {
            return false;
        }
        
        return true;
    }
    
    // Check if DTN can help with specific destination - PUBLIC for Router access
    bool shouldInterceptLocalDM(NodeNum dest) const {
        if (!configEnabled) return false;
        
        // Never intercept broadcasts - DTN is for unicast only
        if (dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA) {
            return false;
        }
        
        // No DTN nodes known - use native DM immediately
        if (fwplusVersionByNode.empty()) {
            return false;
        }
        
        // Direct neighbor - native DM is faster, don't use DTN overlay
        if (isDirectNeighbor(dest)) {
            return false;
        }
        
        // Check if any known DTN node can help reach destination
        return canDtnHelpWithDestination(dest);
    }

  protected:
    // Scheduler tick
    virtual int32_t runOnce() override;
    // Handle incoming DTN protobufs (DATA/RECEIPT) and conservative capture policy
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg) override;
    // Only accept our private FW+ DTN port while enabled
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        if (!p) return false;
        if (!isEnabled()) return false; //fw+ allow full disable via ModuleConfig
        
        // Accept our private FW+ DTN port
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            p->decoded.portnum == meshtastic_PortNum_FWPLUS_DTN_APP) return true;
            
        // fw+ Also accept OnDemand packets for DTN discovery
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            p->decoded.portnum == meshtastic_PortNum_ON_DEMAND_APP) return true;
            
        // Hard-disable capture of foreign unicasts by default
        return false;
    }

  private:
    struct Pending {
        meshtastic_FwplusDtnData data;
        uint32_t nextAttemptMs = 0;
        uint8_t tries = 0;
        //fw+ Loop detection: track recent carriers (circular buffer)
        uint32_t lastCarrier = 0; // node num of last hop we heard from (backward compat)
        NodeNum recentCarriers[3] = {0, 0, 0}; // last 3 carriers for loop detection
        uint8_t recentCarrierIndex = 0; // circular buffer index
        // shortlist of FW+ handoff targets (fixed small array)
        NodeNum handoffCandidates[3] = {0, 0, 0};
        uint8_t handoffCount = 0;
        uint8_t handoffIndex = 0;
        // fw+ tracking failed DTN attempts to detect unresponsive FW+ destinations
        uint8_t dtnFailedAttempts = 0; // consecutive failed DTN overlay attempts
        uint32_t lastDtnAttemptMs = 0; // timestamp of last DTN attempt
        bool fallbackTriggered = false; // flag to avoid re-triggering fallback
    };
    std::unordered_map<uint32_t, Pending> pendingById; // key: orig_id
    // Process received FW+ DTN DATA; deliver locally or schedule and coordinate with peers
    void handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d);
    // Process received FW+ DTN RECEIPT; clear pending, tombstone, interpret reason codes
    void handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r);
    // Create or refresh pending entry and compute election timing
    void scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d);
    // Perform one forwarding decision for a pending DTN item
    void tryForward(uint32_t id, Pending &p);
    // Send compact receipt (status/milestone/expire)
    void emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason = 0);
    // Lightweight FW+ presence probe
    void maybeProbeFwplus(NodeNum dest);
    // Unwrap payload for local delivery
    void deliverLocal(const meshtastic_FwplusDtnData &d);
    // Optionally trigger traceroute to build route confidence
    void maybeTriggerTraceroute(NodeNum dest);
    // Select next hop FW+ neighbor for custody handoff (or return dest if none)
    NodeNum chooseHandoffTarget(NodeNum dest, uint32_t origId, Pending &p);
    // Housekeeping: prune per-destination cache
    void prunePerDestCache();
    // Perform late native DM fallback (encrypted)
    bool sendProxyFallback(uint32_t id, Pending &p);
    // Loop detection helpers
    bool isCarrierLoop(Pending &p, NodeNum carrier) const;
    void trackCarrier(Pending &p, NodeNum carrier);
    
    // Time calculation helpers (avoid code duplication and overflow bugs)
    // Implementations in .cpp to avoid header include dependencies
    uint64_t getEpochMs() const;
    uint64_t calculateTtl(const meshtastic_FwplusDtnData &d) const;
    uint64_t calculateTailStart(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const;
    bool isInTtlTail(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const;

    // Config snapshot
    bool configEnabled = true;
    uint32_t configTtlMinutes = 4; //fw+ dampen overlay window
    uint32_t configInitialDelayBaseMs = 2000; //fw+ reduced from 8000ms for faster delivery
    uint32_t configRetryBackoffMs = 60000;
    uint32_t configMaxTries = 2; //fw+ fewer tries by default
    bool configLateFallback = false;
    uint32_t configFallbackTailPercent = 20;
    bool configMilestonesEnabled = false;
    uint32_t configPerDestMinSpacingMs = 60000; //fw+ wider spacing
    uint32_t configMaxActiveDm = 1; //fw+ default: single active DM
    bool configProbeFwplusNearDeadline = false;
    // Heuristics to reduce airtime while keeping custody
    uint32_t configGraceAckMs = 500;               //fw+ reduced grace period for faster delivery
    uint32_t configSuppressMsAfterForeign = 20000; // backoff when we see someone else carrying same id
    bool configSuppressIfDestNeighbor = true;      // extra conservative for direct neighbor
    bool configPreferBestRouteSlotting = true;     // earlier slotting if DV-ETX confidence
    uint8_t configMilestoneChUtilMaxPercent = 60;  // suppress milestones when channel utilization high
    uint32_t configTombstoneMs = 30000;            // ignore re-captures for this id for a short period
    // Topology-aware throttles
    uint8_t configMaxRingsToAct = 4;               //fw+ increased action radius for mobile nodes
    uint8_t configMilestoneMaxRing = 1;            // milestone only if min(hops to src/dst) <= N
    uint8_t configTailEscalateMaxRing = 2;         // allow tail escalation when within two hops
    // FW+ custody handoff to a direct neighbor when dest is far
    bool configEnableFwplusHandoff = true;
    uint8_t configHandoffMinRing = 2;               // require dest > this many hops to attempt handoff
    uint8_t configHandoffMaxCandidates = 3;         // FW+ neighbors shortlist size
    uint16_t configMinFwplusVersionForHandoff = 45; // require FW+ >= this version
    
    // Periodic FW+ version advertisement with staged warmup
    uint32_t configAdvertiseIntervalMs = 6UL * 60UL * 60UL * 1000UL;  // 6h (normal operation)
    uint32_t configAdvertiseJitterMs = 5UL * 60UL * 1000UL;           // ±5 min
    // Staged warmup: aggressive discovery in first hour, then progressive backoff
    uint32_t configAdvertiseWarmupIntervalMs = 15UL * 60UL * 1000UL;   // 15min (first hour warmup)
    uint32_t configAdvertiseWarmupDurationMs = 60UL * 60UL * 1000UL;   // 1h (warmup phase duration)
    uint8_t configAdvertiseWarmupCount = 4;                            // 4 beacons in warmup (15min × 4 = 60min)
    // Post-warmup: if still no FW+ nodes known, continue searching
    uint32_t configAdvertiseIntervalUnknownMs = 120UL * 60UL * 1000UL; // 2h (post-warmup cold-start)
    // One-shot early advertise after enable/start
    uint32_t configFirstAdvertiseDelayMs = 2UL * 60UL * 1000UL;        // 2min (wait for MQTT proxy connection)
    uint32_t configFirstAdvertiseRetryMs = 10UL * 1000UL;              // 10s retry interval if first beacon fails
    uint32_t configFarMinTtlFracPercent = 20;      //fw+ reduced wait time for far nodes
    uint32_t configOriginProgressMinIntervalMs = 15000; // per-source min interval for milestone
    // Proactive route discovery throttle
    uint32_t configRouteProbeCooldownMs = 5UL * 60UL * 1000UL; // 5 minutes
    // Enhanced monitoring intervals
    uint32_t configDetailedLogIntervalMs = 600000;      // 10 minutes
    // Capture policy (default OFF)
    bool configCaptureForeignEncrypted = false;     // capture of foreign ENCRYPTED DMs
    bool configCaptureForeignText = false;          // capture of foreign TEXT DMs
    
    // Cold start handling
    uint32_t configColdStartTimeoutMs = 30000;      //fw+ timeout before enabling native DM fallback
    bool configColdStartNativeFallback = true;      //fw+ enable native DM when DTN is cold
    
    // Unresponsive FW+ destination fallback
    bool configFwplusUnresponsiveFallback = true;   //fw+ enable fallback when FW+ dest doesn't respond
    uint8_t configFwplusFailureThreshold = 2;       //fw+ consecutive failures before fallback (default 2)
    uint32_t configFwplusResponseTimeoutMs = 180000; //fw+ timeout to expect receipt (3 min)
    // Adaptive milestone limiter
    bool configMilestoneAutoLimiterEnabled = true;       // enable adaptive suppression
    uint8_t configMilestoneAutoSuppressHighChUtil = 55;  // suppress when >= this
    uint8_t configMilestoneAutoReleaseLowChUtil = 30;    // re-enable when <= this
    uint8_t configMilestoneAutoNeighborHigh = 3;         // suppress if we have >= this many neighbors
    uint8_t configMilestoneAutoPendingHigh = 8;          // suppress if pending queue is high

    // DTN counters
    uint32_t ctrForwardsAttempted = 0;
    uint32_t ctrFallbacksAttempted = 0;
    uint32_t ctrReceiptsEmitted = 0;
    uint32_t ctrReceiptsReceived = 0;
    uint32_t ctrExpired = 0;
    uint32_t ctrGiveUps = 0;
    uint32_t ctrMilestonesSent = 0;
    uint32_t ctrProbesSent = 0;
    uint32_t ctrFwplusUnresponsiveFallbacks = 0; //fw+ count of fallbacks to unresponsive FW+ destinations
    //fw+ Enhanced metrics (issue #9)
    uint32_t ctrHandoffsAttempted = 0;       // custody handoff to another FW+ node
    uint32_t ctrHandoffCacheHits = 0;        // used cached handoff target
    uint32_t ctrLoopsDetected = 0;           // carrier loop detected and avoided
    uint32_t ctrDeliveredLocal = 0;          // messages delivered to us as destination
    uint32_t ctrDuplicatesSuppressed = 0;    // duplicate deliveries prevented by tombstone
    uint32_t lastForwardMs = 0;
    // Runtime state
    bool runtimeMilestonesSuppressed = false;
    uint32_t lastAdvertiseMs = 0; //fw+
    uint32_t moduleStartMs = 0;   //fw+
    bool firstAdvertiseDone = false; //fw+
    uint32_t firstAdvertiseRetryMs = 0; //fw+ track retry attempts for first beacon
    uint8_t warmupBeaconsSent = 0; //fw+ track warmup beacons sent (staged discovery)
    uint32_t lastDetailedLogMs = 0;
    // Hello-back unicast reply throttling
    bool configHelloBackEnabled = true;
    uint32_t configHelloBackMinIntervalMs = 60UL * 60UL * 1000UL; // per-origin min interval 1h (balanced for network efficiency)
    uint8_t configHelloBackMaxRing = 3; // reply to nodes up to 3 hops away (FW+DTN is alternative software)
    uint32_t configHelloBackJitterMs = 3000; // small jitter on replies

    // Capability cache of FW+ peers
    std::unordered_map<NodeNum, uint32_t> fwplusSeenMs;
    std::unordered_map<NodeNum, uint16_t> fwplusVersionByNode; //fw+
    std::unordered_map<NodeNum, uint32_t> lastDestTxMs; // per-destination last tx time ms (bounded)
    std::unordered_map<NodeNum, uint32_t> lastRouteProbeMs; // last proactive traceroute per dest
    // Per-origin hello-back rate limit state
    std::unordered_map<NodeNum, uint32_t> lastHelloBackToNodeMs;
    // Anti-burst tracking for broadcast fallbacks
    std::unordered_map<uint32_t, uint32_t> lastBroadcastSentMs; // orig_id -> last broadcast time
    //fw+ cache of destinations confirmed as stock (via native ROUTING ACK/NAK), with age
    std::unordered_map<NodeNum, uint32_t> stockKnownMs;
    bool isDestKnownStock(NodeNum n) const {
        auto it = stockKnownMs.find(n);
        if (it == stockKnownMs.end()) return false;
        return (millis() - it->second) <= (24UL * 60UL * 60UL * 1000UL);
    }
    void markFwplusSeen(NodeNum n) { fwplusSeenMs[n] = millis(); }
    bool isFwplus(NodeNum n) const {
        auto it = fwplusSeenMs.find(n);
        if (it == fwplusSeenMs.end()) return false;
        return (millis() - it->second) <= (24 * 60 * 60 * 1000UL);
    }
    // Short-term tombstones per message id
    std::unordered_map<uint32_t, uint32_t> tombstoneUntilMs; // orig_id -> untilMs
    // Per-source milestone rate limit
    std::unordered_map<NodeNum, uint32_t> lastProgressEmitMsBySource;

    // Helper: check if node is currently a direct neighbor (hop=0)
    bool isDirectNeighbor(NodeNum n) const
    {
        uint8_t hops = getHopsAway(n);
        return (hops != 255 && hops == 0);
    }

    uint8_t countDirectNeighbors() const
    {
        uint8_t cnt = 0;
        int totalNodes = nodeDB->getNumMeshNodes();
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->hops_away == 0 && ni->num != nodeDB->getNodeNum()) cnt++;
        }
        return cnt;
    }

    uint8_t getHopsAway(NodeNum n) const
    {
        int totalNodes = nodeDB->getNumMeshNodes();
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->num == n) return ni->hops_away;
        }
        return 255; // unknown
    }


    uint32_t computeAdaptiveGraceMs(int8_t rxSnr, NodeNum dest) const
    {
        uint32_t base = configGraceAckMs;
        if (!base) return 0;
        if (configSuppressIfDestNeighbor && isDirectNeighbor(dest) && rxSnr >= 8) {
            uint32_t extra = base / 2 + 500;
            return base + extra;
        }
        return base;
    }

    // Cap pending queue to avoid memory growth under load
    static constexpr size_t kMaxPendingEntries = 32;
    static constexpr size_t kMaxPerDestCacheEntries = 64;
    uint32_t lastPruneMs = 0;

    // preferred handoff cache per destination
    struct PreferredHandoffEntry {
        NodeNum node = 0;
        uint32_t lastUsedMs = 0;
        uint8_t rotationIndex = 0; //fw+ global rotation counter for this destination
    };
    std::unordered_map<NodeNum, PreferredHandoffEntry> preferredHandoffByDest;
    uint32_t preferredHandoffTtlMs = 6UL * 60UL * 60UL * 1000UL; // 6h TTL

    // DV-ETX gating wrapper - more permissive for mobile nodes
    bool hasSufficientRouteConfidence(NodeNum dest) const;
    
    // Cold start detection - check if DTN has warmed up (knows at least one FW+ node)
    // Used to trigger aggressive discovery and native DM fallback for intermediate nodes
    bool isDtnCold() const {
        // Cold if we don't know any FW+ nodes yet
        // This allows intermediate nodes to use native DM fallback during cold start
        return fwplusVersionByNode.empty();
    }

    // Telemetry-triggered FW+ probe config/state
    bool configTelemetryProbeEnabled = true;
    uint8_t configTelemetryProbeMinRing = 2; // only probe if origin is >= 2 hops away
    uint32_t configTelemetryProbeCooldownMs = 2UL * 60UL * 60UL * 1000UL; // per-origin cooldown 2h
    std::unordered_map<NodeNum, uint32_t> lastTelemetryProbeToNodeMs;

    // Simple FNV-1a 32-bit
    static uint32_t fnv1a32(uint32_t x) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = (x >> (i * 8)) & 0xFFu;
            h ^= b;
            h *= 16777619u;
        }
        return h;
    }

    // Adaptive milestone emission decision
    bool shouldEmitMilestone(NodeNum src, NodeNum dst);

    // Handoff candidate validation helpers (private - access to Pending struct)
    bool isValidHandoffCandidate(NodeNum candidate, NodeNum dest, const Pending &p) const;
    bool isNodeReachable(NodeNum node) const;
    
    // Route invalidation for mobile nodes
    void invalidateStaleRoutes();
    bool isRouteStale(NodeNum dest) const;
    
    // OnDemand response observation for DTN discovery
    void observeOnDemandResponse(const meshtastic_MeshPacket &mp);
    
    // Cold start aggressive discovery
    void triggerAggressiveDiscovery();
    
    // Check if DTN nodes can help reach destination (proximity analysis)
    bool canDtnHelpWithDestination(NodeNum dest) const;
    
    // Check pending messages for potential handoff when new DTN node discovered
    void checkPendingMessagesForHandoff(NodeNum newDtnNode);
    
    // Scheduling helper functions
    uint32_t calculateBaseDelay(const meshtastic_FwplusDtnData &d, const Pending &p) const;
    uint32_t calculateTopologyDelay(const meshtastic_FwplusDtnData &d) const;
    uint32_t calculateMobilitySlot(uint32_t id, const meshtastic_FwplusDtnData &d, const Pending &p) const;
    uint32_t applyPerDestinationSpacing(uint32_t target, NodeNum dest, float mobility) const;
    
    // Forwarding helper functions
    bool shouldUseFallback(const Pending &p) const;
    bool tryNearDestinationFallback(uint32_t id, Pending &p);
    bool tryKnownStockFallback(uint32_t id, Pending &p);
    bool tryIntelligentFallback(uint32_t id, Pending &p);
    bool tryFwplusUnresponsiveFallback(uint32_t id, Pending &p); //fw+ fallback for unresponsive FW+ nodes
    NodeNum selectForwardTarget(Pending &p);
    void adaptiveMobilityManagement();
    void logDetailedStats();

    // Refactored helpers to keep main flows readable
    void triggerTracerouteIfNeededForSource(const Pending &p, bool lowConf);
    void setPriorityForTailAndSource(meshtastic_MeshPacket *mp, const Pending &p, bool isFromSource);
    void applyForeignCarrySuppression(uint32_t id, Pending &p);
    void applyNearDestExtraSuppression(Pending &p, NodeNum dest);
};

extern DtnOverlayModule *dtnOverlayModule; //fw+

#endif // has fwplus_dtn.pb.h



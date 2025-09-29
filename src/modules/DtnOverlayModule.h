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
    void enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                             uint32_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
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
        uint32_t lastForwardAgeSecs;
        bool enabled;
    };
    // Fill snapshot with current counters
    void getStatsSnapshot(DtnStatsSnapshot &out) const;
    // Record observed FW+ version for a node (from receipt.reason)
    void recordFwplusVersion(NodeNum n, uint16_t version);
    // Periodically advertise FW+ version (one-shot early and periodic thereafter)
    void maybeAdvertiseFwplusVersion();

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
        // Hard-disable capture of foreign unicasts by default
        return false;
    }

  private:
    struct Pending {
        meshtastic_FwplusDtnData data;
        uint32_t nextAttemptMs = 0;
        uint8_t tries = 0;
      uint32_t lastCarrier = 0; // node num of last hop we heard from
      // shortlist of FW+ handoff targets (fixed small array)
      NodeNum handoffCandidates[3] = {0, 0, 0};
      uint8_t handoffCount = 0;
      uint8_t handoffIndex = 0;
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

    // Config snapshot
    bool configEnabled = true;
    uint32_t configTtlMinutes = 4; //fw+ dampen overlay window
    uint32_t configInitialDelayBaseMs = 8000;
    uint32_t configRetryBackoffMs = 60000;
    uint32_t configMaxTries = 2; //fw+ fewer tries by default
    bool configLateFallback = false;
    uint32_t configFallbackTailPercent = 20;
    bool configMilestonesEnabled = false;
    uint32_t configPerDestMinSpacingMs = 60000; //fw+ wider spacing
    uint32_t configMaxActiveDm = 1; //fw+ default: single active DM
    bool configProbeFwplusNearDeadline = false;
    // Heuristics to reduce airtime while keeping custody
    uint32_t configGraceAckMs = 0;                 // delay first overlay attempt to allow direct delivery
    uint32_t configSuppressMsAfterForeign = 20000; // backoff when we see someone else carrying same id
    bool configSuppressIfDestNeighbor = true;      // extra conservative for direct neighbor
    bool configPreferBestRouteSlotting = true;     // earlier slotting if DV-ETX confidence
    uint8_t configMilestoneChUtilMaxPercent = 60;  // suppress milestones when channel utilization high
    uint32_t configTombstoneMs = 30000;            // ignore re-captures for this id for a short period
    // Topology-aware throttles
    uint8_t configMaxRingsToAct = 2;               // action radius in hops
    uint8_t configMilestoneMaxRing = 1;            // milestone only if min(hops to src/dst) <= N
    uint8_t configTailEscalateMaxRing = 2;         // allow tail escalation when within two hops
    // FW+ custody handoff to a direct neighbor when dest is far
    bool configEnableFwplusHandoff = true;
    uint8_t configHandoffMinRing = 2;               // require dest > this many hops to attempt handoff
    uint8_t configHandoffMaxCandidates = 3;         // FW+ neighbors shortlist size
    uint16_t configMinFwplusVersionForHandoff = 45; // require FW+ >= this version
    // Periodic FW+ version advertisement
    uint32_t configAdvertiseIntervalMs = 6UL * 60UL * 60UL * 1000UL;  // 6h
    uint32_t configAdvertiseJitterMs = 5UL * 60UL * 1000UL;           // ±5 min
    // One-shot early advertise after enable/start
    uint32_t configFirstAdvertiseDelayMs = 15000;                      // 15s
    uint32_t configFarMinTtlFracPercent = 60;      // far nodes wait longer before acting
    uint32_t configOriginProgressMinIntervalMs = 15000; // per-source min interval for milestone
    // Proactive route discovery throttle
    uint32_t configRouteProbeCooldownMs = 5UL * 60UL * 1000UL; // 5 minutes
    // Capture policy (default OFF)
    bool configCaptureForeignEncrypted = false;     // capture of foreign ENCRYPTED DMs
    bool configCaptureForeignText = false;          // capture of foreign TEXT DMs
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
    uint32_t lastForwardMs = 0;
    // Runtime state
    bool runtimeMilestonesSuppressed = false;
    uint32_t lastAdvertiseMs = 0; //fw+
    uint32_t moduleStartMs = 0;   //fw+
    bool firstAdvertiseDone = false; //fw+

    // Capability cache of FW+ peers
    std::unordered_map<NodeNum, uint32_t> fwplusSeenMs;
    std::unordered_map<NodeNum, uint16_t> fwplusVersionByNode; //fw+
    std::unordered_map<NodeNum, uint32_t> lastDestTxMs; // per-destination last tx time ms (bounded)
    std::unordered_map<NodeNum, uint32_t> lastRouteProbeMs; // last proactive traceroute per dest
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

    // Helper: check if node is currently a direct neighbor
    bool isDirectNeighbor(NodeNum n) const
    {
        int totalNodes = nodeDB->getNumMeshNodes();
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->num == n && ni->hops_away == 0) return true;
        }
        return false;
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
    };
    std::unordered_map<NodeNum, PreferredHandoffEntry> preferredHandoffByDest;
    uint32_t preferredHandoffTtlMs = 6UL * 60UL * 60UL * 1000UL; // 6h TTL

    // DV-ETX gating wrapper
    bool hasSufficientRouteConfidence(NodeNum dest) const {
        if (!router) return true;
        // minimal confidence 1 like S&F
        return router->hasRouteConfidence(dest, 1);
    }

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
};

extern DtnOverlayModule *dtnOverlayModule; //fw+

#endif // has fwplus_dtn.pb.h



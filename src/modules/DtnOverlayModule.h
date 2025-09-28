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
    DtnOverlayModule();
    //fw+ enqueue overlay data created from a captured DM (plaintext or encrypted)
    void enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                             uint32_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                             bool allowProxyFallback);
    //fw+ expose minimal stats snapshot for OnDemand
    size_t getPendingCount() const { return pendingById.size(); }
    //fw+ DTN stats snapshot
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
    void getStatsSnapshot(DtnStatsSnapshot &out) const;

  protected:
    virtual int32_t runOnce() override;
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        if (!p) return false;
        if (!configEnabled) return false; //fw+ allow full disable via ModuleConfig
        // Accept our private FW+ DTN port
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            p->decoded.portnum == meshtastic_PortNum_FWPLUS_DTN_APP) return true;
        // Capture encrypted DMs (non-broadcast)
        if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag && !isBroadcast(p->to)) return true;
        // Capture plaintext DMs on TEXT port (non-broadcast)
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP && !isBroadcast(p->to)) return true;
        return false;
    }

  private:
    struct Pending {
        meshtastic_FwplusDtnData data;
        uint32_t nextAttemptMs = 0;
        uint8_t tries = 0;
      uint32_t lastCarrier = 0; // node num of last hop we heard from
    };
    std::unordered_map<uint32_t, Pending> pendingById; // key: orig_id
    void handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d);
    void handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r);
    void scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d);
    void tryForward(uint32_t id, Pending &p);
    void emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason = 0);
    void maybeProbeFwplus(NodeNum dest);
    void deliverLocal(const meshtastic_FwplusDtnData &d);

    // Config snapshot
    bool configEnabled = true;
    uint32_t configTtlMinutes = 5;
    uint32_t configInitialDelayBaseMs = 8000;
    uint32_t configRetryBackoffMs = 60000;
    uint32_t configMaxTries = 3;
    bool configLateFallback = false;
    uint32_t configFallbackTailPercent = 20;
    bool configMilestonesEnabled = false;
    uint32_t configPerDestMinSpacingMs = 30000;
    uint32_t configMaxActiveDm = 2;
    bool configProbeFwplusNearDeadline = false;
    //fw+ heuristics to reduce airtime while keeping custody
    uint32_t configGraceAckMs = 0;                 // delay first overlay attempt to allow direct delivery
    uint32_t configSuppressMsAfterForeign = 20000; // backoff when we see someone else carrying same id
    bool configSuppressIfDestNeighbor = true;      // if dest is our direct neighbor, be extra conservative
    bool configPreferBestRouteSlotting = true;     // earlier slotting if we have DV-ETX confidence
    uint8_t configMilestoneChUtilMaxPercent = 60;  // suppress milestones when channel utilization above this
    uint32_t configTombstoneMs = 30000;            // ignore re-captures for this id for a short period
    //fw+ topology-aware throttles
    uint8_t configMaxRingsToAct = 1;               // only act if dest is within N hops from us (0=only neighbor)
    uint8_t configMilestoneMaxRing = 1;            // allow milestone only if min(hops to src/dst) <= N
    uint8_t configTailEscalateMaxRing = 1;         // allow DEFAULT priority in TTL tail only if within N hops
    uint32_t configFarMinTtlFracPercent = 50;      // if dest beyond rings, wait until this % of TTL has passed
    uint32_t configOriginProgressMinIntervalMs = 15000; // per-source min interval between milestones
    //fw+ automatic milestone limiter (adaptive to load/topology)
    bool configMilestoneAutoLimiterEnabled = true;       // enable adaptive suppression
    uint8_t configMilestoneAutoSuppressHighChUtil = 55;  // suppress when >= this
    uint8_t configMilestoneAutoReleaseLowChUtil = 30;    // re-enable when <= this
    uint8_t configMilestoneAutoNeighborHigh = 3;         // suppress if we have >= this many neighbors
    uint8_t configMilestoneAutoPendingHigh = 8;          // suppress if pending queue is high

    //fw+ DTN counters for diagnostics
    uint32_t ctrForwardsAttempted = 0;
    uint32_t ctrFallbacksAttempted = 0;
    uint32_t ctrReceiptsEmitted = 0;
    uint32_t ctrReceiptsReceived = 0;
    uint32_t ctrExpired = 0;
    uint32_t ctrGiveUps = 0;
    uint32_t ctrMilestonesSent = 0;
    uint32_t ctrProbesSent = 0;
    uint32_t lastForwardMs = 0;
    //fw+ runtime state for auto milestone limiter
    bool runtimeMilestonesSuppressed = false;

    // Capability cache of FW+ peers
    std::unordered_map<NodeNum, uint32_t> fwplusSeenMs;
    std::unordered_map<NodeNum, uint32_t> lastDestTxMs; // per-destination last tx time ms (bounded)
    void markFwplusSeen(NodeNum n) { fwplusSeenMs[n] = millis(); }
    bool isFwplus(NodeNum n) const {
        auto it = fwplusSeenMs.find(n);
        if (it == fwplusSeenMs.end()) return false;
        return (millis() - it->second) <= (24 * 60 * 60 * 1000UL);
    }
    //fw+ short-term tombstones per message id to avoid re-enqueue storms
    std::unordered_map<uint32_t, uint32_t> tombstoneUntilMs; // orig_id -> untilMs
    //fw+ per-source milestone rate limit
    std::unordered_map<NodeNum, uint32_t> lastProgressEmitMsBySource;

    //fw+ helper: check if node is currently a direct neighbor
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

    //fw+ cap pending queue to avoid memory growth/fragmentation under load
    static constexpr size_t kMaxPendingEntries = 32;
    static constexpr size_t kMaxPerDestCacheEntries = 64;
    uint32_t lastPruneMs = 0;

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

    //fw+ helper: adaptive decision for milestone emission
    bool shouldEmitMilestone(NodeNum src, NodeNum dst);
};

extern DtnOverlayModule *dtnOverlayModule; //fw+

#endif // has fwplus_dtn.pb.h



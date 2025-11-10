#pragma once

//DTN overlay module (FW+ only). Guard compile until protobufs are generated.
#if __has_include("mesh/generated/meshtastic/fwplus_dtn.pb.h")

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/fwplus_dtn.pb.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <vector>
#include <string>

class DtnOverlayModule : private concurrency::OSThread, public ProtobufModule<meshtastic_FwplusDtn>
{
  public:
    static constexpr size_t kDtnPayloadBytes =
        sizeof(((meshtastic_FwplusDtnData_payload_t *)nullptr)->bytes);

    // Construct DTN overlay module
    DtnOverlayModule();
    // Apply latest ModuleConfig.dtn_overlay at runtime (hot-reload)
    void reloadFromModuleConfig();
    // Query helpers for other subsystems
    bool isEnabled() const { return configEnabled; } //fw+
    uint32_t getTtlMinutes() const { return configTtlMinutes; } //fw+
    // Enqueue overlay data created from a captured DM (plaintext or encrypted)
    //Create DTN envelope from captured DM using relative TTL
    // ttlMinutes: Time-to-live in minutes (converted to ms internally)
    // Returns: true if packet was enqueued, false if skipped (tombstone, queue full, too large)
    bool enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                             uint32_t ttlMinutes, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                             bool allowProxyFallback);
    // Expose DTN stats snapshot for diagnostics
    size_t getPendingCount() const { return pendingById.size(); }
    struct DtnStatsSnapshot {
        // Basic stats
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
        uint32_t knownNodesCount; //number of known FW+ nodes
        bool enabled;
        // Custody & handoff statistics
        uint32_t handoffsAttempted;
        uint32_t handoffCacheHits;
        uint32_t loopsDetected;
        // Local delivery statistics
        uint32_t deliveredLocal;
        uint32_t duplicatesSuppressed;
        // Progressive relay statistics
        uint32_t progressiveRelays;
        uint32_t progressiveRelayLoops;
        // Adaptive routing statistics
        uint32_t adaptiveReroutes;
        uint32_t linkHealthChecks;
        uint32_t pathLearningUpdates;
        uint32_t monitoredLinks;
        uint32_t monitoredPaths;
        // Fallback statistics
        uint32_t fwplusUnresponsiveFallbacks; //count of fallbacks to unresponsive FW+ nodes
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
    // NOTE: Router calls with channel parameter, must validate!
    bool shouldInterceptLocalDM(NodeNum dest, uint8_t channel = 0) const {
        if (!configEnabled) {
            LOG_DEBUG("DTN intercept: disabled");
            return false;
        }
        
        //Skip encrypted packets (channel=0x1f) - Router may call before decryption
        if (channel == 0x1f || channel > MAX_NUM_CHANNELS) {
            LOG_DEBUG("DTN intercept SKIP: encrypted/invalid channel=%u", channel);
            return false;
        }
        
        // Never intercept broadcasts - DTN is for unicast only
        if (dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA) {
            LOG_DEBUG("DTN intercept: broadcast");
            return false;
        }
        
        // No DTN nodes known - use native DM immediately
        if (fwplusVersionByNode.empty()) {
            LOG_DEBUG("DTN intercept: no FW+ nodes known");
            return false;
        }
        
        // Direct neighbor - native DM is faster, don't use DTN overlay
        if (isDirectNeighbor(dest)) {
            LOG_INFO("DTN intercept SKIP: dest 0x%x is direct neighbor", (unsigned)dest);
            return false;
        }
        
        //CRITICAL: Skip DTN for known stock destinations (avoid useless intercept + fallback)
        // After first fallback to stock dest, future messages skip DTN entirely
        if (isDestKnownStock(dest)) {
            LOG_INFO("DTN intercept SKIP: dest 0x%x is known stock (use native DM)", (unsigned)dest);
            return false;
        }
        
        // Check if any known DTN node can help reach destination
        bool canHelp = canDtnHelpWithDestination(dest);
        LOG_INFO("DTN intercept: dest 0x%x canHelp=%d known_fwplus=%u", 
                 (unsigned)dest, (int)canHelp, (unsigned)fwplusVersionByNode.size());
        return canHelp;
    }
    
    //REMOVED FIX #105c: checkTombstone() was unused (FIX #106b handles re-capture)
    
    //FIX #108: Make emitReceipt public for PhoneAPI access
    // Purpose: Allow PhoneAPI to send DTN ACCEPTED receipt after intercept
    // Usage: PhoneAPI calls this after DTN intercepts a message from phone
    //FIX #116: Added custodyPath parameter for return path optimization
    void emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason = 0, 
                     uint8_t custodyPathCount = 0, const uint8_t *custodyPath = nullptr,
                     uint32_t origRemainingTtlMs = 0);

  protected:
    // Scheduler tick
    virtual int32_t runOnce() override;
    // Handle incoming DTN protobufs (DATA/RECEIPT) and conservative capture policy
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg) override;
    // Only accept our private FW+ DTN port while enabled
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        if (!p) return false;
        if (!isEnabled()) return false; //allow full disable via ModuleConfig
        
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
        uint32_t lastAttemptMs = 0; //Track last forward time for TTL decrement
        uint32_t captureTimeMs = 0; //Track capture time for absolute timeout
        //Loop detection: track recent carriers (circular buffer)
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
        // Progressive relay tracking (for unknown/distant destinations)
        // FIX #86: Increase progressive relay chain limit from 3 to 5 (matches FIX #84 max_tries)
        // PROBLEM: FIX #84 allowed 5 attempts but progressiveRelays[3] only tracked 3 edge nodes
        //          Result: Attempts 4-5 had no edge nodes available → fallback triggered prematurely
        // SOLUTION: Match array size to extended max_tries for full edge node rotation
        NodeNum progressiveRelays[5] = {0, 0, 0, 0, 0}; // Track relay nodes used (loop prevention)
        uint8_t progressiveRelayCount = 0; // Number of progressive relay hops taken
        //FIX #178: Cache progressive relay choice to prevent double-call bug
        // chooseProgressiveRelay was called twice per attempt → first call added node to progressiveRelays[]
        // → second call skipped it as "already tried" → selected wrong edge node!
        NodeNum cachedProgressiveRelay = 0; // Cached result from tryForward, used by selectForwardTarget
        //FIX #116: Force specific edge for return path optimization
        // PROBLEM: DELIVERED receipts use forward path logic → wrong route → dropped
        // SOLUTION: For receipts, use reverse custody_path as edge (return via same route)
        NodeNum forceEdge = 0; // If non-zero, selectForwardTarget uses this edge
        //Return-chain hints propagated inside custody receipts for deterministic routing
        uint8_t receiptReturnChain[16] = {0};
        uint8_t receiptReturnLen = 0;
        uint8_t receiptReturnIndex = 0;
        bool hasReceiptReturnChain = false;
        uint8_t receiptSelfIndex = 0;
        uint8_t receiptHopRetries = 0;
        uint32_t receiptHopStartMs = 0;
        bool isReceiptVariant = false;
        meshtastic_FwplusDtnReceipt receiptMsg = meshtastic_FwplusDtnReceipt_init_zero;
        uint32_t receiptOrigId = 0;
        bool usePki = false;
        bool disablePki = false;
        bool hasPlainPayload = false;
        pb_size_t plainPayloadSize = 0;
        uint8_t plainPayload[kDtnPayloadBytes] = {0};
        bool originalIsEncrypted = false;
        //NEW: Track broadcast fallback attempts to prevent broadcast storms
        // PROBLEM: Far destinations (5+ hops) with no viable custody paths trigger broadcast on EVERY retry
        //          Example: 18 broadcasts in 4.5 minutes for single packet = massive airtime waste
        // SOLUTION: Limit broadcast fallbacks to max 4 attempts per packet
        //           After 4 broadcasts, wait for custody timeout without further broadcasts
        uint8_t broadcastFallbackCount = 0; // Number of broadcast fallbacks sent for this packet
        //FIX #168: Flag receipt custody to disable progressive relay on intermediate nodes
        // PROBLEM: Receipts use progressive relay → suboptimal paths → packet loss
        //          Example: Node13 → Node12 → Node31 (progressive) vs Node13 → Node12 → Node1 (direct)
        // SOLUTION: Mark receipt custody, use learned chain/routing table instead of progressive relay
        // BENEFIT: Optimal return paths, higher delivery rate for DELIVERED receipts
        bool isReceiptCustody = false; // True if this pending entry represents a receipt custody packet
        bool isReceiptSource = false;  // True if this receipt was emitted by this node
    };
    std::unordered_map<uint32_t, Pending> pendingById; // key: orig_id
    
    //FIX #115 + FIX #158: Track receipt custody IDs for broadcast fallback
    // FIX #115 (original): DELIVERED receipts only - erase after first attempt
    // FIX #158 (revised): ALL receipts (DELIVERED, PROGRESSED, EXPIRED, FAILED)
    //                     Trigger broadcast fallback after try=1 if unicast fails
    // BENEFIT: Reliable receipt delivery even with unreachable reverse paths
    std::unordered_set<uint32_t> deliveredReceiptIds; // Receipt custody_ids (broadcast fallback after try=1)
    std::unordered_map<NodeNum, uint32_t> pkiRetryAfterMs; // cooldown for nodes lacking PKI handshake
    
    // Process received FW+ DTN DATA; deliver locally or schedule and coordinate with peers
    void handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d);
    // Process received FW+ DTN RECEIPT; clear pending, tombstone, interpret reason codes
    void handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r);
    // Create or refresh pending entry and compute election timing
    void scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d);
    // Perform one forwarding decision for a pending DTN item
    void tryForward(uint32_t id, Pending &p);
    // Lightweight FW+ presence probe
    void maybeProbeFwplus(NodeNum dest);
    // Unwrap payload for local delivery
    bool deliverLocal(const meshtastic_FwplusDtnData &d);
    // Optionally trigger traceroute to build route confidence
    void maybeTriggerTraceroute(NodeNum dest);
    // Select next hop FW+ neighbor for custody handoff (or return dest if none)
    NodeNum chooseHandoffTarget(NodeNum dest, uint32_t origId, Pending &p);
    // Select furthest known FW+ node for progressive relay (unknown/distant destinations)
    NodeNum chooseProgressiveRelay(NodeNum dest, Pending &p);
    //map DV-ETX next_hop byte to full NodeNum (handles sparse NodeDB ordering)
    NodeNum resolveNextHopNode(uint8_t nextHopByte) const;
    NodeNum findDirectFwplusNeighbor(const Pending &p);  //FIX #96: Last resort fallback
    // Housekeeping: prune per-destination cache
    void prunePerDestCache();
    // Perform late native DM fallback (encrypted)
    bool sendProxyFallback(uint32_t id, Pending &p);
    // Loop detection helpers
    bool isCarrierLoop(Pending &p, NodeNum carrier) const;
    void trackCarrier(Pending &p, NodeNum carrier);
    NodeNum applyReceiptReturnHop(Pending &p, uint8_t targetIdx, bool allowStale = false);
    bool degradeReceiptReturnHop(uint32_t id, Pending &p);
    bool refreshReceiptReturnHints(uint32_t origId, const meshtastic_FwplusDtnData &d);
    bool cleanupPendingFromReceipt(const meshtastic_FwplusDtnData &receipt, const char *contextLabel);
    
    // Time calculation helpers (avoid code duplication and overflow bugs)
    //TTL helpers
    uint64_t getEpochMs() const; // Legacy - kept for compatibility
    bool isInTtlTail(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const;

    // Config snapshot
    bool configEnabled = true;
    uint32_t configTtlMinutes = 8; //dampen overlay window
    uint32_t configInitialDelayBaseMs = 2000; //reduced from 8000ms for faster delivery
    uint32_t configRetryBackoffMs = 60000;
    uint32_t configMaxTries = 2; //fewer tries by default
    bool configLateFallback = false;
    uint32_t configFallbackTailPercent = 20;
    bool configMilestonesEnabled = true;
    uint32_t configPerDestMinSpacingMs = 60000; //wider spacing
    uint32_t configNextHopMinSpacingMs = 9000;   // minimum spacing per next-hop to smooth bursts [ms]
    uint32_t configMaxActiveDm = 3; //default: single active DM
    bool configProbeFwplusNearDeadline = false;
    bool configEnableE2ePki = true; // enable end-to-end PKI wrapping of payloads when possible
    uint32_t configPkiRetryDelayMs = 5UL * 60UL * 1000UL; // retry PKI after 5 minutes by default
    // Minimum suppression window for duplicate local deliveries after first delivery
    uint32_t configDeliveredTombstoneMs = 30UL * 60UL * 1000UL; // 30 minutes
    // Heuristics to reduce airtime while keeping custody
    uint32_t configGraceAckMs = 500;               //reduced grace period for faster delivery
    uint32_t configSuppressMsAfterForeign = 20000; // backoff when we see someone else carrying same id
    bool configSuppressIfDestNeighbor = true;      // extra conservative for direct neighbor
    bool configPreferBestRouteSlotting = true;     // earlier slotting if DV-ETX confidence
    uint8_t configMilestoneChUtilMaxPercent = 60;  // suppress milestones when channel utilization high
    uint32_t configTombstoneMs = 30000;            // ignore re-captures for this id for a short period
    // Receipt TTL policy
    // PROGRESSED: short telemetry lifetime; EXPIRED: brief notification; DELIVERED/FAILED: longer for reliability
    uint32_t configReceiptTtlProgressedMs = 60000;      // 60s default
    uint32_t configReceiptTtlExpiredMs = 90000;         // 90s default
    uint32_t configReceiptTtlDeliveredFailedMin = 5;    // 5 minutes default
    // Cap receipts relative to remaining DATA TTL plus slack to avoid very late receipts
    uint32_t configReceiptTtlSlackMs = 60000;           // +60s beyond remaining DATA TTL
    // Topology-aware throttles
    uint8_t configMaxRingsToAct = 4;               //increased action radius for mobile nodes
    uint8_t configMilestoneMaxRing = 1;            // milestone only if min(hops to src/dst) <= N
    uint8_t configTailEscalateMaxRing = 2;         // allow tail escalation when within two hops
    // FW+ custody handoff to a direct neighbor when dest is far
    bool configEnableFwplusHandoff = true;
    uint8_t configHandoffMinRing = 2;               // require dest > this many hops to attempt handoff
    uint8_t configHandoffMaxCandidates = 3;         // FW+ neighbors shortlist size
    //FIX #167: Raise minimum version to 75 (FNV-1a hash + receipt cleanup)
    // PROBLEM: FW+ v<75 has critical bugs:
    //          - XOR hash collision (FIX #165) → custody_id conflicts → DELIVERED receipts lost
    //          - Receipt accumulation (FIX #166) → memory leak, pending queue grows indefinitely
    // SOLUTION: Require v75+ for custody handoff (prevent old nodes from custody chain)
    // BENEFIT: Network stability, no hash collisions, no receipt accumulation
    // NOTE: Old nodes can still RECEIVE (graceful degradation), but won't be used for custody
    uint16_t configMinFwplusVersionForHandoff = 81; // require FW+ >= this version
    
    // Periodic FW+ version advertisement with staged warmup
    uint32_t configAdvertiseIntervalMs = 6UL * 60UL * 60UL * 1000UL;  // 6h (normal operation)
    uint32_t configAdvertiseJitterMs = 5UL * 60UL * 1000UL;           // ±5 min
    // Staged warmup: aggressive discovery in first 70min (4 beacons guaranteed), then progressive backoff
    uint32_t configAdvertiseWarmupIntervalMs = 15UL * 60UL * 1000UL;   // 15min (warmup interval)
    uint32_t configAdvertiseWarmupDurationMs = 70UL * 60UL * 1000UL;   // 70min (warmup phase duration - extended to guarantee 4 beacons with max jitter)
    uint8_t configAdvertiseWarmupCount = 4;                            // 4 beacons in warmup (15min × 4 + jitter margin)
    // Post-warmup: if still no FW+ nodes known, continue searching
    uint32_t configAdvertiseIntervalUnknownMs = 120UL * 60UL * 1000UL; // 2h (post-warmup cold-start)
    // One-shot early advertise after enable/start
    #ifdef ARCH_PORTDUINO
    uint32_t configFirstAdvertiseDelayMs = 10UL * 1000UL;              // 10s for simulator (fast discovery)
    #else
    uint32_t configFirstAdvertiseDelayMs = 2UL * 60UL * 1000UL;        // 2min base + 0-60s random jitter (prevents sync burst after power outage)
    #endif
    uint32_t configFirstAdvertiseRetryMs = 10UL * 1000UL;              // 10s retry interval if first beacon fails
    uint32_t configFarMinTtlFracPercent = 20;      //reduced wait time for far nodes
    uint32_t configOriginProgressMinIntervalMs = 15000; // per-source min interval for milestone
    // Proactive route discovery throttle
    uint32_t configRouteProbeCooldownMs = 15UL * 60UL * 1000UL; // 15 minutes
    uint32_t configReceiptMaxNodeAgeSec = 60UL * 60UL;         //receipts require fresh neighbors (1h staleness)
    // Enhanced monitoring intervals
    uint32_t configDetailedLogIntervalMs = 600000;      // 10 minutes
    // Capture policy (default OFF)
    bool configCaptureForeignEncrypted = false;     // capture of foreign ENCRYPTED DMs
    bool configCaptureForeignText = false;          // capture of foreign TEXT DMs
    
    // Cold start handling
    uint32_t configColdStartTimeoutMs = 30000;      //timeout before enabling native DM fallback (30s for testing)
    bool configColdStartNativeFallback = true;      //enable native DM when DTN is cold
    
    // Unresponsive FW+ destination fallback
    bool configFwplusUnresponsiveFallback = true;   //enable fallback when FW+ dest doesn't respond
    uint8_t configFwplusFailureThreshold = 2;       //consecutive failures before fallback (default 2)
    uint32_t configFwplusResponseTimeoutMs = 300000; //timeout to expect receipt (5 min for distant multi-hop chains)
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
    uint32_t ctrFwplusUnresponsiveFallbacks = 0; //count of fallbacks to unresponsive FW+ destinations
    uint32_t ctrHandoffsAttempted = 0;       // custody handoff to another FW+ node
    uint32_t ctrHandoffCacheHits = 0;        // used cached handoff target
    uint32_t ctrLoopsDetected = 0;           // carrier loop detected and avoided
    uint32_t ctrDeliveredLocal = 0;          // messages delivered to us as destination
    uint32_t ctrDuplicatesSuppressed = 0;    // duplicate deliveries prevented by tombstone
    uint32_t ctrProgressiveRelays = 0;       // progressive relay to edge nodes (unknown/distant dest)
    uint32_t ctrProgressiveRelayLoops = 0;   // progressive relay loops detected and prevented
    uint32_t lastForwardMs = 0;
    // Runtime state
    bool runtimeMilestonesSuppressed = false;
    uint32_t lastAdvertiseMs = 0;
    uint32_t moduleStartMs = 0;
    bool firstAdvertiseDone = false;
    uint32_t firstAdvertiseRetryMs = 0; //track retry attempts for first beacon
    uint8_t warmupBeaconsSent = 0; //track warmup beacons sent (staged discovery)
    uint32_t lastDetailedLogMs = 0;
    // Hello-back unicast reply throttling
    bool configHelloBackEnabled = true;
    uint32_t configHelloBackMinIntervalMs = 60UL * 60UL * 1000UL; // per-origin min interval 1h (balanced for network efficiency)
    uint8_t configHelloBackMaxRing = 3; // reply to nodes up to 3 hops away
    uint32_t configHelloBackJitterMs = 3000; // small jitter on replies

    // Capability cache of FW+ peers
    std::unordered_map<NodeNum, uint32_t> fwplusSeenMs;
    std::unordered_map<NodeNum, uint16_t> fwplusVersionByNode;  
    std::unordered_map<NodeNum, uint32_t> lastDestTxMs; // per-destination last tx time ms (bounded)
    std::unordered_map<NodeNum, uint32_t> lastNextHopTxMs; // per-next-hop last tx time ms
    std::unordered_map<NodeNum, uint32_t> lastRouteProbeMs; // last proactive traceroute per dest
    // Per-origin hello-back rate limit state
    std::unordered_map<NodeNum, uint32_t> lastHelloBackToNodeMs;
    //FIX #127: Scheduled hello-backs with random delay (RF storm prevention)
    std::unordered_map<NodeNum, uint32_t> scheduledHelloBackMs; // NodeNum -> scheduled send time (millis)
    // Anti-burst tracking for broadcast fallbacks
    std::unordered_map<uint32_t, uint32_t> lastBroadcastSentMs; // orig_id -> last broadcast time
    //cache of destinations confirmed as stock (via native ROUTING ACK/NAK), with age
    std::unordered_map<NodeNum, uint32_t> stockKnownMs;
    bool isDestKnownStock(NodeNum n) const {
        auto it = stockKnownMs.find(n);
        if (it == stockKnownMs.end()) return false;
        return (millis() - it->second) <= (24UL * 60UL * 60UL * 1000UL);
    }
    
    //FIX #123: Direction hints from relay_node (beacon routing intelligence)
    // Maps destination NodeNum to relay_node that forwarded beacon from that destination
    // This tells us which neighbor is "in the direction" of a distant FW+ node
    std::unordered_map<NodeNum, NodeNum> relayHintsByDest; // dest -> relay_node (direction hint)
    
    //Adaptive routing: cache successful custody chains per destination
    // Stores the first-hop relay that led to successful delivery
    struct SuccessfulChain {
        NodeNum firstHop;      // First relay node in successful chain
        uint32_t learnedMs;    // When we learned this chain
        uint8_t chainLength;   // Total hops in chain (for metrics)
    };
    std::unordered_map<NodeNum, SuccessfulChain> learnedChainsByDest; // dest -> chain
    uint32_t configChainCacheTtlMs = 6 * 60 * 60 * 1000UL; // 6h TTL for learned chains
    void markFwplusSeen(NodeNum n) { fwplusSeenMs[n] = millis(); }
    void certifyFwplus(NodeNum n, const char *context = nullptr);
    //FIX #155: Comprehensive FW+ detection (checks both maps)
    // PROBLEM: isFwplus() only checked fwplusSeenMs (nodes that sent DTN DATA/RECEIPT)
    //          Result: Nodes discovered via beacons (fwplusVersionByNode) not detected as FW+
    // SOLUTION: Check BOTH fwplusSeenMs (24h TTL) AND fwplusVersionByNode (beacon discovery)
    // BENEFIT: Consistent FW+ detection, aligns with sendProxyFallback logic (line 2486-2488)
    bool isFwplus(NodeNum n) const {
        // Check recent activity (DATA/RECEIPT packets)
        auto itSeen = fwplusSeenMs.find(n);
        if (itSeen != fwplusSeenMs.end() && 
            (millis() - itSeen->second) <= (24 * 60 * 60 * 1000UL)) {
            return true;
        }
        // Also check version map (beacon discovery)
        auto itVer = fwplusVersionByNode.find(n);
        return (itVer != fwplusVersionByNode.end());
    }
    static constexpr uint32_t kMinReasonVersion = 81; // Minimum value identifying a FW+ version advertisement
    bool isVersionReason(uint32_t reason) const { return reason >= kMinReasonVersion; }

    // Short-term tombstones per message id
    std::unordered_map<uint32_t, uint32_t> tombstoneUntilMs; // orig_id -> untilMs
    // Map original message id -> original destination for DV-ETX seeding on delivery receipts
    std::unordered_map<uint32_t, NodeNum> sentDestByOrigId;
    // IDs for which we've already emitted a local implicit ACK upon overhearing custody forward
    std::unordered_set<uint32_t> overhearAckedIds;
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

    //FIX #88: Check if Router has reliable routing to destination
    // PROBLEM: NodeDB has hops_away but Router may have no next_hop (routing table mismatch)
    // SOLUTION: Use route confidence as proxy for Router routing availability
    //           Low confidence indicates Router has no reliable path despite NodeDB distance info
    bool hasRouterNextHop(NodeNum dest) const
    {
        // If we have sufficient route confidence, Router has reliable next_hop
        // Low confidence (despite known hops) indicates NodeDB/Router mismatch
        return hasSufficientRouteConfidence(dest);
    }

    NodeNum resolveLowByteToNodeNum(uint8_t lowByte) const;

    uint32_t countActiveDataPendings() const;

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
    static constexpr size_t kMaxSentMapEntries = 128;
    uint32_t lastPruneMs = 0;

    // preferred handoff cache per destination
    struct PreferredHandoffEntry {
        NodeNum node = 0;
        uint32_t lastUsedMs = 0;
        uint8_t rotationIndex = 0; //global rotation counter for this destination
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
    
    // Global probe rate limiter to prevent bursts
    // Ensures MINIMUM 90 seconds between ANY probes (not just per-node)
    uint32_t lastGlobalProbeMs = 0;
    uint32_t configGlobalProbeMinIntervalMs = 90000; // Absolute minimum 90s between probes
    uint32_t configMaxNodeAgeSec = 24UL * 60UL * 60UL; // Only probe nodes seen within 24h (not dead nodes)
    

    bool isGlobalProbeCooldownActive() const {
        if (lastGlobalProbeMs == 0) return false;
        uint32_t nowMs = millis();
        return (nowMs > lastGlobalProbeMs) && ((nowMs - lastGlobalProbeMs) < configGlobalProbeMinIntervalMs);
    }
    
    void updateGlobalProbeTimestamp() {
        lastGlobalProbeMs = millis();
    }
    
    uint32_t getGlobalProbeCooldownRemainingSec() const {
        if (!isGlobalProbeCooldownActive()) return 0;
        uint32_t nowMs = millis();
        uint32_t elapsedMs = nowMs - lastGlobalProbeMs;
        return (configGlobalProbeMinIntervalMs - elapsedMs) / 1000;
    }
    
    // Check if node is "alive" (seen recently via last_heard)
    bool isNodeAlive(NodeNum node) const;
    
    // Get node age in seconds (time since last_heard)
    uint32_t getNodeAgeSec(NodeNum node) const;

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
    bool isNodeReachable(NodeNum node, uint32_t maxAgeSecOverride = 0) const;
    
    // Route invalidation for mobile nodes
    void invalidateStaleRoutes();
    bool isRouteStale(NodeNum dest) const;
    
    // OnDemand response observation for DTN discovery
    void observeOnDemandResponse(const meshtastic_MeshPacket &mp);

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
    bool tryFwplusUnresponsiveFallback(uint32_t id, Pending &p); //fallback for unresponsive FW+ nodes
    NodeNum selectForwardTarget(Pending &p);
    void adaptiveMobilityManagement();
    void logDetailedStats();

    // Helpers for DV-ETX seeding and tracking
    void trackSentDestForOrigId(uint32_t origId, NodeNum dest);
    void seedDvEtxFromReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r);

    // Refactored helpers to keep main flows readable
    void triggerTracerouteIfNeededForSource(const Pending &p, bool lowConf);
    void setPriorityForTailAndSource(meshtastic_MeshPacket *mp, const Pending &p, bool isFromSource);
    void applyForeignCarrySuppression(uint32_t id, Pending &p);
    void applyNearDestExtraSuppression(Pending &p, NodeNum dest);
    
    // handleReceivedProtobuf helper functions for improved readability
    bool checkAndSuppressDuplicateNativeText(const meshtastic_MeshPacket &mp);
    void processMilestoneEmission(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &data);
    bool processForeignDMCapture(const meshtastic_MeshPacket &mp);
    void processTelemetryProbe(const meshtastic_MeshPacket &mp);

    //==============================================================================
    // ADAPTIVE PATH SELECTION & FAULT TOLERANCE
    //==============================================================================

    // Purpose: Track health metrics for a specific neighbor link
    // Used to: Make intelligent routing decisions and avoid problematic links
    struct LinkHealth {
        NodeNum neighbor;                 // Neighbor node ID
        uint32_t successCount;            // Total successful transmissions
        uint32_t failureCount;            // Total failed transmissions
        uint8_t consecutiveFailures;      // Current streak of failures (reset on success)
        float avgSnr;                     // Average SNR (Exponentially Weighted Moving Average)
        float avgRssi;                    // Average RSSI (EWMA)
        uint32_t lastSuccessMs;           // Timestamp of last successful transmission
        uint32_t lastFailureMs;           // Timestamp of last failure
        uint32_t lastHealthCheckMs;       // Last time health was evaluated
        
        LinkHealth() : neighbor(0), successCount(0), failureCount(0), 
                       consecutiveFailures(0), avgSnr(0.0f), avgRssi(0.0f),
                       lastSuccessMs(0), lastFailureMs(0), lastHealthCheckMs(0) {}
    };

    // Purpose: Track reliability of a specific path (source -> intermediate -> destination)
    // Used to: Learn from failures and avoid problematic routes for future packets
    struct PathReliability {
        NodeNum firstHop;                 // First hop of this path
        NodeNum destination;              // Final destination
        uint32_t successCount;            // Packets successfully delivered via this path
        uint32_t failureCount;            // Packets that failed via this path
        uint32_t lastAttemptMs;           // Last time we tried this path
        uint32_t lastSuccessMs;           // Last successful delivery
        uint32_t lastFailureMs;           // Last failure
        bool temporarilyBlocked;          // Is this path currently blocked?
        uint32_t blockExpiryMs;           // When to unblock this path (0 = not blocked)
        
        PathReliability() : firstHop(0), destination(0), successCount(0), failureCount(0),
                            lastAttemptMs(0), lastSuccessMs(0), lastFailureMs(0),
                            temporarilyBlocked(false), blockExpiryMs(0) {}
        
        // Calculate success rate (0.0 to 1.0)
        float getSuccessRate() const {
            uint32_t total = successCount + failureCount;
            return (total > 0) ? (float)successCount / total : 0.5f; // Unknown = neutral
        }
        
        // Check if path should be avoided
        bool isUnreliable() const {
            if (temporarilyBlocked && millis() < blockExpiryMs) {
                return true; // Blocked until expiry
            }
            
            // Consider unreliable if:
            // - More than 3 consecutive failures
            // - Success rate below 40%
            // - No success in last 5 minutes despite attempts
            bool tooManyFailures = (failureCount > 3 && getSuccessRate() < 0.4f);
            bool staleFailures = (lastFailureMs > lastSuccessMs && 
                                  (millis() - lastSuccessMs) > 300000 &&
                                  failureCount > 0);
            
            return tooManyFailures || staleFailures;
        }
    };

    // Purpose: Track which paths were attempted for a specific packet
    // Used to: Avoid retrying same failed path and enable learning
    struct PacketPathHistory {
        PacketId packetId;                    // Original packet ID
        NodeNum destination;                  // Destination of this packet
        std::vector<NodeNum> attemptedHops;   // First hops we tried
        uint8_t pathSwitchCount;              // How many times we switched paths
        uint32_t firstAttemptMs;              // When we first tried
        uint32_t lastAttemptMs;               // Last attempt timestamp
        
        PacketPathHistory() : packetId(0), destination(0), pathSwitchCount(0),
                              firstAttemptMs(0), lastAttemptMs(0) {}
        
        // Check if we already tried this first hop
        bool hasAttempted(NodeNum hop) const {
            return std::find(attemptedHops.begin(), attemptedHops.end(), hop) 
                   != attemptedHops.end();
        }
        
        // Record a new attempt
        void recordAttempt(NodeNum hop) {
            if (!hasAttempted(hop)) {
                attemptedHops.push_back(hop);
                pathSwitchCount++;
            }
            lastAttemptMs = millis();
            if (firstAttemptMs == 0) firstAttemptMs = millis();
        }
    };

    // Helper struct for path candidates
    struct PathCandidate {
        NodeNum nextHop;
        uint8_t hopCount;
        float cost;
        float score;
        
        PathCandidate() : nextHop(0), hopCount(255), cost(999.0f), score(0.0f) {}
    };

    // Link health monitoring functions
    void updateLinkHealth(NodeNum neighbor, bool success, float snr, float rssi);
    bool isLinkHealthy(NodeNum neighbor) const;
    float getLinkQualityScore(NodeNum neighbor) const;
    void maintainLinkHealth();

    // Path reliability learning functions
    void recordPathAttempt(NodeNum firstHop, NodeNum destination, PacketId packetId);
    void updatePathReliability(NodeNum firstHop, NodeNum destination, bool success);
    bool isPathReliable(NodeNum firstHop, NodeNum destination) const;
    std::set<NodeNum> getUnreliablePaths(NodeNum destination) const;
    void maintainPathReliability();

    // Adaptive path selection functions
    NodeNum selectAlternativePathOnFailure(uint32_t id, Pending &p, NodeNum failedHop = 0);
    float calculatePathScore(NodeNum firstHop, NodeNum dest, float cost, uint8_t hops) const;
    

    // Maintenance functions
    void runPeriodicMaintenance();
    void logAdaptiveRoutingStatistics();
    //FIX #127: Process scheduled hello-backs with random delay
    void processScheduledHellobacks();
    void pruneTombstones();
    std::string formatHopList(const std::vector<NodeNum>& hops) const;

    // Link health monitoring
    std::map<NodeNum, LinkHealth> linkHealthMap;
    uint32_t linkHealthUpdateIntervalMs = 30000;  // Update health every 30s
    
    // Path reliability learning
    std::map<std::pair<NodeNum, NodeNum>, PathReliability> pathReliabilityMap;
    
    // Per-packet path tracking
    std::map<PacketId, PacketPathHistory> packetPathHistoryMap;
    
    // Configuration
    bool configEnableAdaptiveRerouting = true;
    uint8_t configMaxPathSwitches = 2;           // Max reroute attempts per packet
    uint8_t configLinkFailureThreshold = 3;      // Consecutive fails to mark link bad
    uint32_t configLinkHealthWindowMs = 300000;  // 5min window for health calc
    uint32_t configPathBlockDurationMs = 600000; // 10min path block on failure
    float configMinLinkSnrThreshold = 3.0f;      // Minimum acceptable SNR
    float configMinPathSuccessRate = 0.3f;       // Minimum acceptable path success rate
    
    // Statistics
    uint32_t ctrAdaptiveReroutes = 0;            // Counter: adaptive path switches
    uint32_t ctrLinkHealthChecks = 0;            // Counter: link health evaluations
    uint32_t ctrPathLearningUpdates = 0;         // Counter: path reliability updates
    uint32_t lastMaintenanceMs = 0;              // Last maintenance timestamp
};

extern DtnOverlayModule *dtnOverlayModule; //fw+

#endif // has fwplus_dtn.pb.h



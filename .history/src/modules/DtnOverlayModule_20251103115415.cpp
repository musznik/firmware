/*
//DTN Overlay Module — Overview (DTN-first custody, FW+ handoff)

Purpose
- Opportunistic store–carry–forward for private Direct Messages. With DTN enabled, private TEXT unicasts are intercepted
  at the sender and wrapped as FWPLUS_DTN DATA (DTN-first). Native DM is used as an intelligent fallback for stock
  destinations or late in TTL. Stock nodes require no changes; FW+ nodes unwrap at the destination and inject the original DM locally.

Performance Baseline
- Delivery rate: ~100% (0-6 hops)
- Short distance (0-3 hops): <30s via direct handoff or Router
- Long distance (4-6 hops): 30-90s via progressive relay custody chain
- Channel utilization: 3-18% (excellent for long-distance mesh)
- Custody handoff to direct neighbors only (hops_away=0) ensures reliable Router unicast
- Progressive relay for hops>3 enables 4-6 hop reach beyond Router hop_limit=3
- Extended max_tries (5) and chain depth (5) for deep mesh penetration
- Retry backoff: Fixed 40s

Ingress sources
- Local send interception (DTN-first): Router diverts private TEXT to a DTN queue with a TTL; no immediate native DM.
- FWPLUS_DTN DATA overheard on the mesh: always accepted and scheduled; if we are the destination we unwrap and deliver.
- Optional capture of foreign (non-DTN) unicasts is conservative (default OFF) to avoid DTN-on-DTN recursion.
- OnDemand observation: passively discovers DTN-enabled nodes from OnDemand responses and records their versions.
- Telemetry-triggered probes: optionally probes unknown nodes (≥2 hops) when observing telemetry/nodeinfo (default OFF).

Scheduling
- First attempt: immediate for source (50-150ms), fast path for direct neighbors (50ms base, 25ms slots); intermediates use 
  deterministic slotting and per-destination spacing. DV‑ETX gating is more permissive for mobile nodes. Far nodes are delayed.
- Source optimization: zero slot delay for source's first attempt ensures immediate transmission when DTN can help.
- Adaptive parameters: mobility factor adjusts retry backoff, spacing, and max attempts dynamically.

Forwarding and Handoff
- Custody handoff to direct neighbors only (hops_away=0) for reliable Router unicast
- Progressive relay for hops>3 creates custody chain via edge nodes beyond hop_limit
- Shortlist up to 3 FW+ direct neighbors, rotate across retries
- Optimistic handoff: when route unknown use closest FW+ neighbors
- Global loop detection: custody_path tracks chain (source→relay1→relay2→...), prevents loops
- Local loop detection: recentCarriers[3] prevents immediate re-handoff
- Broadcast fallback: after retry failure or in TTL tail, unencrypted, cooldown 60-120s
- Priority: BACKGROUND default, escalate to DEFAULT in TTL tail or when source
- Global active cap and per-destination spacing limit concurrency

Receipts & Milestones
- Receipts (DELIVERED/PROGRESSED/EXPIRED) clear pending and set tombstone
- PROGRESSED emitted sparingly, ring-gated with hysteresis under load
- Version advertisement via 32-bit reason field (lower 16 bits)
- deliverLocal() uses custody_path_count and rx_snr=-128.0f synthetic marker

Traceroute & Fallback
- Source: traceroute for known routes with low confidence only
- Unknown routes: progressive relay immediately, no traceroute defer
- Known routes with routing issues: Router next_hop check detects mismatch, use progressive relay
- Stock destination: early native DM fallback for nearby, DTN custody for distant
- Intermediates: forward immediately, coordinate via suppression
- Broadcast fallback: TTL tail only (20%), strict cooldown and channel util gating
- Unresponsive FW+: track failed attempts, trigger native DM fallback after N failures + timeout
- Late fallback (optional): native DM in TTL tail when progressive relay exhausted

Version Advertisement & Discovery
- One-shot public beacon after start, then staged periodic beacons (15min → 2h → 6h)
- Hello-back: unicast version up to 3 hops, rate-limited
- Passive discovery: OnDemand responses, overheard DTN DATA/RECEIPTs
- Dynamic re-evaluation: new DTN nodes trigger pending message rescheduling
- Intelligent interception: local DMs intercepted only when DTN can help reach destination

Mobility Adaptation
- Route invalidation based on last_heard and mobility factor
- Adaptive timeouts: 15-30min (mobile) vs 2h (stationary)
- Mobility-aware scheduling: reduced spacing (60%), earlier attempts, adjusted backoff

Airtime protections
- Channel utilization gate, deterministic election, mobility-aware slotting
- Per-destination spacing, global active cap, suppression after foreign DATA
- Near-destination extra suppression (2-3x) prevents duplicate deliveries
- Tombstones and bounded caches with pruning

Example Scenarios (Mixed Network: Stock + FW+ Nodes)

Scenario 1: Direct path with DTN relay
  Alice(FW+) --2h-- Bob(FW+) --3h-- Charlie(FW+) --1h-- Dave(Stock)
  - Alice sends DM to Dave → intercepted, wrapped as DTN DATA (DTN-first)
  - Bob carries packet, forwards DTN DATA toward Dave (custody maintained)
  - Charlie (1-hop from Dave) receives DTN, unwraps and injects native DM to Dave
  - Dave receives normal DM, no firmware changes needed
  - Charlie sends DELIVERED receipt back to Alice → mission complete

Scenario 2: DTN nodes off main path (side relay)
  Alice(FW+) --2h-- Bob(Stock) --3h-- Charlie(Stock)
       |
      1h
       |
    Emma(FW+) --2h-- Frank(FW+)
  - Alice sends DM to Charlie (far, low confidence)
  - Alice hands off custody to Emma (closer FW+ neighbor, better topology knowledge)
  - Emma carries packet, later encounters Frank who has route to Charlie
  - Frank receives handoff, forwards toward Charlie using native DM fallback (stock destination)
  - Result: DTN provided alternative path when direct route was weak

Scenario 3: Mobile messenger (store-carry-forward)
  Alice(FW+) --5h-- [gap] --4h-- Bob(Stock)
       \
        Emma(FW+, mobile) moves between areas
  - Alice sends DM to Bob → low confidence, far destination
  - Alice hands custody to Emma (mobile node, may encounter better routes)
  - Emma carries packet while moving, topology changes
  - Emma gets closer to Bob's area → unwraps and sends native DM
  - Mobility adaptation: shorter retry timeouts, adjusted spacing

Scenario 4: Cold start (no DTN knowledge yet)
  Alice(FW+, just booted) --2h-- Bob(FW+) --1h-- Charlie(Stock)
  - Alice sends DM to Charlie but doesn't know Bob is FW+ yet (cold start)
  - Alice uses native DM fallback (conservative, avoids black hole)
  - Alice triggers aggressive discovery → probes neighbors
  - Bob responds with FW+ version beacon → Alice learns Bob is DTN-capable
  - Next message: Alice can use DTN handoff to Bob

Scenario 5: Unresponsive FW+ fallback
  Alice(FW+) --2h-- Bob(FW+, offline) --1h-- Charlie(Stock)
  - Alice sends DM to Charlie, attempts DTN handoff to Bob
  - Bob doesn't respond (offline/version mismatch) after N attempts + timeout
  - Alice detects unresponsive FW+ → switches to native DM fallback
  - Message reaches Charlie via stock routing
  - Later: Bob sends beacon → Alice clears "stock" marking, DTN re-enabled

Key Behaviors in Mixed Networks:
- FW+ nodes intercept local DMs only when DTN can help (topology/proximity check)
- Stock destinations get native DM fallback in TTL tail (seamless compatibility)
- Intermediates suppress after hearing foreign DATA (coordination, avoid storms)
- Receipts provide end-to-end confirmation and passive FW+ discovery
- Broadcast as last resort: public, cooldown-gated, allows 7-hop propagation
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
#include "MobilityOracle.h"
#include "modules/RoutingModule.h"
#include "mesh/NextHopRouter.h"
#include "mesh/CryptoEngine.h"
#include "mesh/RadioInterface.h"
#include "FwPlusVersion.h"
#include "mesh/generated/meshtastic/ondemand.pb.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mqtt/MQTT.h"
#include <pb_encode.h>
#include <cstring>
#include <sstream>
#include <set>
#include <algorithm>
#include <cmath>

DtnOverlayModule *dtnOverlayModule; 
// Purpose: hot-reload DTN overlay settings from ModuleConfig at runtime.
// Notes: respects zero-as-default semantics; clears pending queue if module gets disabled.
void DtnOverlayModule::reloadFromModuleConfig()
{
    if (!moduleConfig.has_dtn_overlay) return;
    const auto &mc = moduleConfig.dtn_overlay;
    bool wasEnabled = configEnabled;
    // copy fields with guards (respect zeros meaning defaults where applicable)
    configEnabled = mc.enabled;
    if (mc.ttl_minutes) configTtlMinutes = mc.ttl_minutes; else configTtlMinutes = configTtlMinutes;
    if (mc.initial_delay_base_ms) configInitialDelayBaseMs = mc.initial_delay_base_ms;
    if (mc.retry_backoff_ms) configRetryBackoffMs = mc.retry_backoff_ms;
    if (mc.max_tries) configMaxTries = mc.max_tries;
    configLateFallback = mc.late_fallback_enabled;
    if (mc.fallback_tail_percent) configFallbackTailPercent = mc.fallback_tail_percent;
    configMilestonesEnabled = mc.milestones_enabled;
    if (mc.per_dest_min_spacing_ms) configPerDestMinSpacingMs = mc.per_dest_min_spacing_ms;
    if (mc.max_active_dm) configMaxActiveDm = mc.max_active_dm;
    configProbeFwplusNearDeadline = mc.probe_fwplus_near_deadline;

    // If just disabled, clear pending to stop activity immediately
    if (wasEnabled && !configEnabled) {
        pendingById.clear();
    }
}

void DtnOverlayModule::recordFwplusVersion(NodeNum n, uint16_t version)
{
    bool wasNew = (fwplusVersionByNode.find(n) == fwplusVersionByNode.end());

    fwplusVersionByNode[n] = version;
    markFwplusSeen(n);

    auto itStock = stockKnownMs.find(n);
    if (itStock != stockKnownMs.end()) {
        stockKnownMs.erase(itStock);
        LOG_INFO("DTN: Clearing stock marking for node 0x%x (FW+ version %u)",
                 (unsigned)n, (unsigned)version);
    }

    if (nodeDB) {
        if (meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(n)) {
            uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
            if (nowEpoch != 0) {
                info->last_heard = nowEpoch;
            }
        }
    }

    if (wasNew) {
        LOG_INFO("DTN: Discovered FW+ node 0x%x ver=%u (total FW+ nodes: %u)",
                 (unsigned)n, (unsigned)version, (unsigned)fwplusVersionByNode.size());
    } else {
        LOG_DEBUG("DTN: Updated FW+ node 0x%x to version %u", (unsigned)n, (unsigned)version);
    }
}

//fw+
void DtnOverlayModule::certifyFwplus(NodeNum n, const char *context)
{
    if (n == 0 || n == NODENUM_BROADCAST || n == NODENUM_BROADCAST_NO_LORA) return;
    if (nodeDB && n == nodeDB->getNodeNum()) return;

    markFwplusSeen(n);

    auto stockIt = stockKnownMs.find(n);
    if (stockIt != stockKnownMs.end()) {
        stockKnownMs.erase(stockIt);
        LOG_INFO("DTN: Clearing stock marking for node 0x%x via %s",
                 (unsigned)n, context ? context : "FW+ evidence");
    }

    auto [itVer, inserted] = fwplusVersionByNode.emplace(n, (uint16_t)FW_PLUS_VERSION);
    if (!inserted && itVer->second < configMinFwplusVersionForHandoff) {
        itVer->second = (uint16_t)FW_PLUS_VERSION;
    }

    if (inserted) {
        LOG_INFO("DTN: Marked node 0x%x as FW+ via %s (placeholder ver=%u)",
                 (unsigned)n, context ? context : "evidence", (unsigned)FW_PLUS_VERSION);
    } else {
        LOG_DEBUG("DTN: Reinforced FW+ evidence for node 0x%x via %s",
                  (unsigned)n, context ? context : "evidence");
    }
}

bool DtnOverlayModule::shouldEmitMilestone(NodeNum src, NodeNum dst)
{
    (void)src;
    (void)dst;

    if (!configMilestonesEnabled) {
        return false;
    }

    if (!configMilestoneAutoLimiterEnabled) {
        return true;
    }

    if (runtimeMilestonesSuppressed) {
        return false;
    }

    return true;
}

//fw+
NodeNum DtnOverlayModule::resolveLowByteToNodeNum(uint8_t lowByte) const
{
    if (!nodeDB) return 0;
    int totalNodes = nodeDB->getNumMeshNodes();
    for (int i = 0; i < totalNodes; ++i) {
        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
        if (!ni) continue;
        if ((ni->num & 0xFFu) == lowByte) {
            return ni->num;
        }
    }
    return 0;
}

// Purpose: unwrap received DTN payload for local delivery (destination == us).
// Behavior: injects original DM to local Router as encrypted or decoded variant, preserving original sender id.
bool DtnOverlayModule::deliverLocal(const meshtastic_FwplusDtnData &d)
{
    // FIX #85: Calculate realistic hop distance from custody chain length
    // PROBLEM: hop_start=0 magic marker prevented NodeDB from updating hops_away
    //          New nodes got hops_away=0 (protobuf default) → false "direct neighbor" detection
    //          Result: Asymmetric delivery (B→N worked via DTN, N→B failed - thought B was direct!)
    // SOLUTION: Use realistic hop_start based on custody chain length for correct NodeDB routing
    //           Use rx_snr=-128.0f as new circular loop marker (impossible LoRa value)
    uint8_t estimatedHops = d.custody_path_count > 0 ? d.custody_path_count : 3;  // Minimum 3 if no chain

    auto sendDecodedToPhone = [&](const uint8_t *buf, size_t len, const char *tag) {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        size_t copyLen = (len > sizeof(p->decoded.payload.bytes)) ? sizeof(p->decoded.payload.bytes) : len;
        if (copyLen > 0) memcpy(p->decoded.payload.bytes, buf, copyLen);
        p->decoded.payload.size = copyLen;
        p->channel = d.channel;
        p->want_ack = false;
        p->hop_start = estimatedHops;
        p->hop_limit = 0;
        p->rx_snr = -128.0f;
        LOG_INFO("DTN: deliverLocal(%s) hops=%u id=0x%x to=0x%x from=0x%x (chain_len=%u)",
                 tag, (unsigned)estimatedHops, (unsigned)p->id, (unsigned)p->to,
                 (unsigned)p->from, (unsigned)d.custody_path_count);
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    };

    if (d.use_pki_encryption) {
        meshtastic_NodeInfoLite *srcInfo = nodeDB->getMeshNode(d.orig_from);
        if (!srcInfo || srcInfo->user.public_key.size != 32) {
            LOG_WARN("DTN: Missing source public key for PKI payload id=0x%x from=0x%x", d.orig_id, d.orig_from);
            emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                       meshtastic_Routing_Error_PKI_FAILED, d.custody_path_count, d.custody_path);
            return false;
        }

        if (d.payload.size <= MESHTASTIC_PKC_OVERHEAD) {
            LOG_WARN("DTN: PKI payload too small id=0x%x size=%u", d.orig_id, (unsigned)d.payload.size);
            emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                       meshtastic_Routing_Error_PKI_FAILED, d.custody_path_count, d.custody_path);
            return false;
        }

        uint8_t plaintext[sizeof(d.payload.bytes)] = {0};
        bool decrypted = crypto->decryptCurve25519(d.orig_from, srcInfo->user.public_key, d.orig_id,
                                                  d.payload.size, d.payload.bytes, plaintext);
        if (!decrypted) {
            LOG_WARN("DTN: PKI decrypt failed id=0x%x from=0x%x", d.orig_id, d.orig_from);
            emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                       meshtastic_Routing_Error_PKI_FAILED, d.custody_path_count, d.custody_path);
            memset(plaintext, 0, sizeof(plaintext));
            return false;
        }

        size_t plainLen = d.payload.size - MESHTASTIC_PKC_OVERHEAD;
        sendDecodedToPhone(plaintext, plainLen, "PKI");
        memset(plaintext, 0, sizeof(plaintext));
        return true;
    }

    if (d.is_encrypted) {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(p->encrypted.bytes, d.payload.bytes, d.payload.size);
        p->encrypted.size = d.payload.size;
        p->channel = d.channel;
        p->want_ack = false; //DTN uses RECEIPT, not native ACK (eliminates redundancy)
        p->hop_start = estimatedHops;
        p->hop_limit = 0;
        p->rx_snr = -128.0f;
        LOG_INFO("DTN: deliverLocal(ENCRYPTED) hops=%u id=0x%x to=0x%x from=0x%x (chain_len=%u)",
                 (unsigned)estimatedHops, (unsigned)p->id, (unsigned)p->to, (unsigned)p->from, (unsigned)d.custody_path_count);
        service->sendToMesh(p, RX_SRC_LOCAL, false);
        return true;
    }

    //FIX #158: Detect DTN receipt binary payload leaked to deliverLocal()
    if (d.payload.size >= 2 && d.payload.bytes[0] == 0xAC && d.payload.bytes[1] == 0xDC) {
        LOG_ERROR("DTN FIX #158: deliverLocal() received receipt binary payload (magic 0xAC 0xDC) - aborting!");
        LOG_ERROR("This should NOT happen! Receipt detection failed. Payload size=%u, channel=%u, encrypted=%d",
                 d.payload.size, d.channel, d.is_encrypted);
        return false;
    }

    sendDecodedToPhone(d.payload.bytes, d.payload.size, "PLAINTEXT");
    return true;
}

// Purpose: export a consistent read-only snapshot of DTN runtime counters and flags.
void DtnOverlayModule::getStatsSnapshot(DtnStatsSnapshot &out) const
{
    // Basic stats
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
    
    // Count active FW+ nodes (seen within last 24h)
    uint32_t knownCount = 0;
    for (const auto &kv : fwplusSeenMs) {
        if ((now - kv.second) <= (24 * 60 * 60 * 1000UL)) {
            knownCount++;
        }
    }
    out.knownNodesCount = knownCount;
    
    // Custody & handoff statistics
    out.handoffsAttempted = ctrHandoffsAttempted;
    out.handoffCacheHits = ctrHandoffCacheHits;
    out.loopsDetected = ctrLoopsDetected;
    
    // Local delivery statistics
    out.deliveredLocal = ctrDeliveredLocal;
    out.duplicatesSuppressed = ctrDuplicatesSuppressed;
    
    // Progressive relay statistics
    out.progressiveRelays = ctrProgressiveRelays;
    out.progressiveRelayLoops = ctrProgressiveRelayLoops;
    
    // Adaptive routing statistics
    out.adaptiveReroutes = ctrAdaptiveReroutes;
    out.linkHealthChecks = ctrLinkHealthChecks;
    out.pathLearningUpdates = ctrPathLearningUpdates;
    out.monitoredLinks = linkHealthMap.size();
    out.monitoredPaths = pathReliabilityMap.size();
    
    // Fallback statistics
    out.fwplusUnresponsiveFallbacks = ctrFwplusUnresponsiveFallbacks;
}

// Purpose: initialize DTN overlay module with conservative defaults and read ModuleConfig.
DtnOverlayModule::DtnOverlayModule()
    : concurrency::OSThread("DtnOverlay"),
      ProtobufModule("FwplusDtn", meshtastic_PortNum_FWPLUS_DTN_APP, &meshtastic_FwplusDtn_msg)
{
    //enable promiscuous sniffing and acceptance of encrypted packets for overlay capture
    isPromiscuous = true;
    encryptedOk = true;

    //read config with sensible defaults (moduleConfig.dtn_overlay may not exist yet; use defaults)
    //clarify: documented purpose/units for defaults below
    configEnabled = false; // Default: disabled (will be set by env/moduleConfig below)
    configTtlMinutes = 6; // DTN custody TTL [minutes] for overlay items; balanced window for multi-hop delivery
    configInitialDelayBaseMs = 2000; //reduced base delay before first attempt [ms] for faster delivery
    //soften: larger retry backoff to reduce overlay traffic rate
    configRetryBackoffMs = 40000; // FIX #46: 40s balanced for mesh (was 120s)
    configMaxTries = 3; // FIX #36: Increased from 2 to 3 for better long-path reliability
    configLateFallback = false; // enable late native-DM fallback near TTL tail
    configFallbackTailPercent = 20; // start fallback in the last X% of TTL [%]
    configMilestonesEnabled = false; // emit sparse PROGRESSED milestones (telemetry); default OFF
    //soften: larger per-destination spacing to avoid bursts
    configPerDestMinSpacingMs = 60000; // per-destination minimum spacing between attempts [ms] (1 min, max uint16_t=65535)
    configMaxActiveDm = 2; // global cap of active DTN attempts per scheduler pass
    configProbeFwplusNearDeadline = false; // send lightweight FW+ probe near TTL tail before fallback
    //conservative airtime heuristics
    configGraceAckMs = 500;                   //reduced grace window for faster delivery [ms]
    configSuppressMsAfterForeign = 35000;     // suppression after hearing foreign overlay DATA [ms] (be polite)
    configSuppressIfDestNeighbor = true;      // add extra delay when destination is our direct neighbor
    configPreferBestRouteSlotting = true;     // start earlier if DV-ETX route confidence is good
    
    // Enhanced monitoring
    configDetailedLogIntervalMs = 600000;      // 10 minutes
    lastDetailedLogMs = 0;
 
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
#ifdef ARCH_PORTDUINO
    // FIX #47: Force test config for simulator (override any saved NodeDB settings)
    // This ensures our test values are used regardless of NodeDB/prefs state
    LOG_WARN("DTN: FORCING simulator config (override NodeDB): backoffMs=40000 maxTries=3");
    //configRetryBackoffMs = 40000;  // FIX #46: 40s balanced
   // configMaxTries = 3;             // FIX #36: 3 attempts
   // configPerDestMinSpacingMs = 10000;  // 10s spacing for faster tests
   // configMaxActiveDm = 3;          // Higher concurrency for testing
    
    //Allow simulator PKI regression testing when DTN_PRESERVE_REGION=1
    const char *preserveRegionEnv = getenv("DTN_PRESERVE_REGION");
    const bool preserveRegion = preserveRegionEnv && atoi(preserveRegionEnv) != 0;

    if (preserveRegion) {
        //Force simulator PKI runs to a known-good region (US)
        if (config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_US) {
            LOG_WARN("DTN: Preserving region with US override for PKI tests (prev=%u)", static_cast<unsigned>(config.lora.region));
            config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
        } else {
            LOG_WARN("DTN: Preserving region=US (PKI test mode via DTN_PRESERVE_REGION)");
        }
    } else {
        //FIX #148: Disable PKI for simulator (UNSET region = no PKI keygen, prevents key collisions)
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        LOG_WARN("DTN: Forcing region=UNSET (disable PKI for simulator tests)");
    }
    
    // Milestones provide freshness for reverse paths; keep them enabled by default in simulator runs
    const char *milestoneEnv = getenv("DTN_MILESTONES");
    if (milestoneEnv != nullptr) {
        configMilestonesEnabled = (atoi(milestoneEnv) != 0);
        LOG_WARN("DTN: Node 0x%x milestones %s via env DTN_MILESTONES=%s", nodeDB->getNodeNum(),
                 configMilestonesEnabled ? "ENABLED" : "DISABLED", milestoneEnv);
    } else if (!moduleConfig.has_dtn_overlay || moduleConfig.dtn_overlay.milestones_enabled) {
        configMilestonesEnabled = true;
        LOG_WARN("DTN: Enabling milestones by default for simulator (fresh reverse paths)");
    } else {
        LOG_WARN("DTN: Milestones disabled via moduleConfig (simulator override respected)");
    }
    
    // Per-node DTN enable/disable for mixed network testing
    // Must be AFTER moduleConfig to override saved settings
    // Priority: 1) Environment variable, 2) Config file, 3) moduleConfig/default
    char envVar[64];
    snprintf(envVar, sizeof(envVar), "DTN_ENABLED_NODE_%u", nodeDB->getNodeNum() & 0xFF);
    const char* dtnEnv = getenv(envVar);
    
    if (dtnEnv != nullptr) {
        // Explicit env var control (highest priority)
        configEnabled = (atoi(dtnEnv) != 0);
        LOG_WARN("DTN: Node 0x%x DTN %s via env %s=%s (OVERRIDE moduleConfig)", 
                 nodeDB->getNodeNum(), configEnabled ? "ENABLED" : "DISABLED", envVar, dtnEnv);
    } else {
        // Try config file (Windows/WSL compatibility)
        char configPath[128];
        snprintf(configPath, sizeof(configPath), "/mnt/c/tmp/dtn_config_node%u.txt", nodeDB->getNodeNum() & 0xFF);
        
        FILE* f = fopen(configPath, "r");
        if (f != nullptr) {
            char value[4] = {0};
            if (fgets(value, sizeof(value), f) != nullptr) {
                configEnabled = (atoi(value) != 0);
                LOG_WARN("DTN: Node 0x%x DTN %s via config file %s (OVERRIDE moduleConfig)", 
                         nodeDB->getNodeNum(), configEnabled ? "ENABLED" : "DISABLED", configPath);
            }
            fclose(f);
        } else {
            // No env/config - use moduleConfig or default to enabled for simulator
            if (!moduleConfig.has_dtn_overlay) {
                configEnabled = true; // Default for simulator (backward compat)
                LOG_DEBUG("DTN: Node 0x%x using default DTN=enabled (no env/config/moduleConfig)", nodeDB->getNodeNum());
            }
            // else: configEnabled already set from moduleConfig (line 387)
        }
    }
#endif
    LOG_INFO("DTN init: enabled=%d ttl_min=%u initDelayMs=%u backoffMs=%u maxTries=%u lateFallback=%d tail%%=%u milestones=%d perDestMinMs=%u maxActive=%u probeNearDeadline=%d graceAckMs=%u suppressForeignMs=%u neighborSuppress=%d preferBestRoute=%d maxRings=%u milestoneMaxRing=%u tailEscMaxRing=%u farMinTtl%%=%u",
             (int)configEnabled, (unsigned)configTtlMinutes, (unsigned)configInitialDelayBaseMs,
             (unsigned)configRetryBackoffMs, (unsigned)configMaxTries, (int)configLateFallback,
             (unsigned)configFallbackTailPercent, (int)configMilestonesEnabled,
             (unsigned)configPerDestMinSpacingMs, (unsigned)configMaxActiveDm,
             (int)configProbeFwplusNearDeadline,
             (unsigned)configGraceAckMs, (unsigned)configSuppressMsAfterForeign,
             (int)configSuppressIfDestNeighbor, (int)configPreferBestRouteSlotting,
             (unsigned)configMaxRingsToAct, (unsigned)configMilestoneMaxRing, (unsigned)configTailEscalateMaxRing,
             (unsigned)configFarMinTtlFracPercent);
    
    // Add random jitter 0-60s to first beacon to prevent synchronization when multiple nodes start simultaneously
    // (e.g., after power outage) - this spreads discovery traffic across time
    uint32_t firstBeaconJitter = (uint32_t)random(60001); // 0-60000ms (0-60s)
    configFirstAdvertiseDelayMs += firstBeaconJitter;
    
    LOG_INFO("DTN: Module created, first beacon in %u ms (base 2min + random %u sec jitter)", 
             (unsigned)configFirstAdvertiseDelayMs, (unsigned)(firstBeaconJitter / 1000));
    LOG_INFO("DTN: FW_PLUS_VERSION=%d", FW_PLUS_VERSION);
    LOG_INFO("DTN: Cold start timeout: %u ms, native fallback: %s",
             (unsigned)configColdStartTimeoutMs, configColdStartNativeFallback ? "enabled" : "disabled");
    LOG_INFO("DTN: FW+ unresponsive fallback: %s, threshold: %u failures, timeout: %u ms",
             configFwplusUnresponsiveFallback ? "enabled" : "disabled",
             (unsigned)configFwplusFailureThreshold, (unsigned)configFwplusResponseTimeoutMs);
    
    // Initialize adaptive routing
    lastMaintenanceMs = millis();
    LOG_INFO("DTN: Adaptive rerouting: %s, max path switches: %u, link failure threshold: %u",
             configEnableAdaptiveRerouting ? "enabled" : "disabled",
             (unsigned)configMaxPathSwitches, (unsigned)configLinkFailureThreshold);

    // NOTE: Immediate neighbor probing removed - rely exclusively on passive broadcast beacons
    
    moduleStartMs = millis();
    LOG_DEBUG("DTN: enabled, configEnabled=%d", (int)configEnabled);
}
// Purpose: single scheduler tick; triggers forwards whose time arrived and performs periodic maintenance.
// Returns: milliseconds until next desired wake (clamped 100..2000 ms when enabled).
int32_t DtnOverlayModule::runOnce()
{
    if (!configEnabled) return 1000; //disabled: idle
    
    uint32_t now = millis();
    
    // NOTE: Immediate neighbor probing removed - discovery via passive broadcast beacons only
    
    // Debug: log pending count
    static uint32_t lastDebugMs = 0;
    if (pendingById.size() > 0 && (now - lastDebugMs) > 5000) {
        LOG_INFO("DTN runOnce: %u pending messages", (unsigned)pendingById.size());
        for (const auto& kv : pendingById) {
            LOG_INFO("  Pending id=0x%x next=%u ms", kv.first, 
                     (unsigned)(kv.second.nextAttemptMs > now ? kv.second.nextAttemptMs - now : 0));
        }
        lastDebugMs = now;
    }
    
    // NOTE: Cold start re-probing removed - rely exclusively on passive broadcast beacons
    // Discovery timeline:
    //   - First beacon @ ~2min
    //   - Warmup beacons @ 15min intervals (4 total in 70min, guaranteed with max jitter)
    //   - Post-warmup @ 2h if no FW+ discovered
    //   - Normal @ 6h when FW+ nodes known
    
    //periodically advertise our FW+ version for passive discovery
    maybeAdvertiseFwplusVersion();
    //periodically invalidate stale routes for mobile nodes
    invalidateStaleRoutes();
    //detailed logging and monitoring
    logDetailedStats();
    
    //Random delayed hello-backs (prevents RF collision storm)
    processScheduledHellobacks();
    
    //simple scheduler: attempt forwards whose time arrived
    //Check TTL expiry for all pending packets
    uint32_t dmIssuedThisPass = 0; // reset per scheduler pass
    //dynamic wake: compute nearest nextAttempt across pendings
    uint32_t nextWakeMs = 2000;
    for (auto it = pendingById.begin(); it != pendingById.end();) {
        uint32_t currentId = it->first; // Save ID before tryForward() might invalidate iterator
        Pending &p = it->second;
        
        // Check TTL before tryForward() to avoid use-after-free
        if (p.data.ttl_remaining_ms == 0) {
            // TTL expired - emit EXPIRED receipt to source and drop
            LOG_WARN("DTN expire id=0x%x ttl_remaining=0", currentId);
            emitReceipt(p.data.orig_from, currentId, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0);
            ctrExpired++;
            //create tombstone to avoid immediate milestone after expiry if others still carry it
            if (configTombstoneMs) tombstoneUntilMs[currentId] = millis() + configTombstoneMs;
            it = pendingById.erase(it);
            continue; // Iterator already advanced by erase()
        }
        
        // Global concurrency cap: throttle attempts
        if (now >= p.nextAttemptMs) {
            if (dmIssuedThisPass < configMaxActiveDm) {
                // FIX: Iterator invalidation by emitReceipt() in tryForward()
                // PROBLEM: tryForward() calls emitReceipt() → enqueueFromCaptured() → std::map::insert()
                //          This INVALIDATES iterator even if size doesn't change or increases!
                //          Example: size=5 → emitReceipt adds entry (size=6) → erase old (size=5)
                //          Old check: "if (size < sizeBefore)" → FALSE → crash on ++it!
                // ROOT CAUSE: std::map::insert() invalidates ALL iterators (not just for erased keys)
                //             Checking only size decrease is insufficient
                // SOLUTION: Detect ANY size change (insert OR erase) and restart iteration
                // BENEFIT: Prevents crash when tryForward() modifies pendingById in any way
                size_t sizeBefore = pendingById.size();
                tryForward(currentId, p);
                dmIssuedThisPass++;

                //ensure iterator stays valid even if size didn't change but currentId was erased
                auto refreshed = pendingById.find(currentId);
                if (refreshed == pendingById.end()) {
                    //unordered_map is unordered; restart iteration from begin
                    it = pendingById.begin();
                    continue;
                }

                // Check if tryForward() modified pendingById (iterator protection)
                if (pendingById.size() != sizeBefore) {
                    // pendingById was modified (insert OR erase) - iterator is invalidated
                    // We must restart iteration to be safe
                    it = refreshed;
                    ++it;
                    continue;
                }
            } else {
                // push a bit forward
                p.nextAttemptMs = now + 1000 + (uint32_t)random(500);
                LOG_DEBUG("DTN defer(id=0x%x): glob cap reached, next in %u ms", currentId, (unsigned)(p.nextAttemptMs - now));
                LOG_DEBUG("DTN: Rate limit check: perDestMinSpacingMs=%u, globalActiveCap=%u", configPerDestMinSpacingMs, configMaxActiveDm);
                uint32_t wait = p.nextAttemptMs - now;
                if (wait < nextWakeMs) nextWakeMs = wait;
            }
        } else {
            uint32_t wait = p.nextAttemptMs - now;
            if (wait < nextWakeMs) nextWakeMs = wait;
        }
        
        ++it; // Safe to advance - entry wasn't removed
    }
    //bounded maintenance of per-destination cache to avoid growth
    if (millis() - lastPruneMs > 30000) {
        lastPruneMs = millis();
        prunePerDestCache();
    }
    
    // Periodic maintenance for adaptive routing (link health, path learning, etc.)
    runPeriodicMaintenance();
    
    //clamp next wake between 100..2000 ms to avoid tight/long sleeps
    if (nextWakeMs < 100) nextWakeMs = 100;
    if (nextWakeMs > 2000) nextWakeMs = 2000;
    return (int32_t)nextWakeMs;
}
// Purpose: bound memory use of per-destination last-tx cache by removing oldest entries.
// Policy: prune down to 75% of configured cap using simple aging.
void DtnOverlayModule::prunePerDestCache()
{
    if (lastDestTxMs.size() <= kMaxPerDestCacheEntries) return;
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
//REMOVED FIX #105c: checkTombstone() - unused, FIX #106b handles re-capture
// Purpose: check if node is "alive" (seen recently via last_heard)
// Returns: true if node was seen within configMaxNodeAgeSec (24h default)
// Usage: Rate limiter for telemetry probes (avoid spam to dead nodes)
bool DtnOverlayModule::isNodeAlive(NodeNum node) const
{
    meshtastic_NodeInfoLite *nodeInfo = nodeDB->getMeshNode(node);
    if (!nodeInfo) return false;
    
    // Time-based liveness check
    // CONTEXT: This is used to block telemetry probes to stale/dead nodes (rate limiter)
    // PROBLEM: No epoch time → block ALL probes → no discovery in no-RTC networks!
    // SOLUTION: Multi-tier approach:
    //   1. If valid epoch time: use precise staleness check (preferred)
    //   2. If no epoch time: trust routing table as "good enough" liveness signal
    //   3. Be conservative: only trust nodes with reasonable hops (0-2)
    
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
    uint32_t lastSeenEpoch = nodeInfo->last_heard;
    
    // Tier 1: Valid epoch time available - use precise staleness check
    if (nowEpoch > 0 && lastSeenEpoch > 0) {
        bool alive = (nowEpoch - lastSeenEpoch) <= configMaxNodeAgeSec;
        
        if (!alive) {
            LOG_DEBUG("DTN: Node 0x%x considered dead (age=%u sec > %u sec)", 
                     (unsigned)node, (unsigned)(nowEpoch - lastSeenEpoch), 
                     (unsigned)configMaxNodeAgeSec);
        }
        
        return alive;
    }
    
    // Tier 2: No valid epoch time - fallback to routing table trust
    // RATIONALE: If node is in routing table with close hops (0-2), it's likely active
    //            Allow probing these nodes even without epoch time
    // CONSERVATIVE: Only hops 0-2 (direct + 1-hop neighbors) to avoid spam
    if (nodeInfo->hops_away <= 2) {
        LOG_DEBUG("DTN: Node 0x%x considered alive via routing table (no epoch, hops=%u)", 
                 (unsigned)node, (unsigned)nodeInfo->hops_away);
        return true; // Trust close nodes in routing table
    }
    
    // Tier 3: No time + far node = block probe (conservative safety)
    LOG_DEBUG("DTN: Node 0x%x considered dead (no epoch time, hops=%u > 2)", 
             (unsigned)node, (unsigned)nodeInfo->hops_away);
    return false;
}

// Purpose: get node age in seconds (time since last_heard)
// Returns: age in seconds, or 0xFFFFFFFF if unknown
uint32_t DtnOverlayModule::getNodeAgeSec(NodeNum node) const
{
    meshtastic_NodeInfoLite *nodeInfo = nodeDB->getMeshNode(node);
    if (!nodeInfo) return 0xFFFFFFFF;
    
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
    uint32_t lastSeenEpoch = nodeInfo->last_heard;
    
    if (nowEpoch == 0 || lastSeenEpoch == 0) return 0xFFFFFFFF;
    return (nowEpoch - lastSeenEpoch);
}


// Purpose: check and suppress duplicate native TEXT messages delivered via DTN
// Returns: true if packet should be consumed (duplicate detected)
bool DtnOverlayModule::checkAndSuppressDuplicateNativeText(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP &&
        mp.to == nodeDB->getNodeNum()) {
        auto itTs = tombstoneUntilMs.find(mp.id);
        if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
            LOG_DEBUG("DTN: Suppressing duplicate native TEXT id=0x%x due to recent DTN delivery", (unsigned)mp.id);
            return true; // consume to prevent duplicate UI delivery
        }
    }
    return false;
}
// Purpose: process milestone emission for overheard DTN DATA packets
// Effect: emits sparse PROGRESSED receipts with rate limiting and topology-based filtering
void DtnOverlayModule::processMilestoneEmission(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &data)
{
    // Guard: don't emit milestones if DTN is disabled (don't advertise as FW+ capable)
    if (!configEnabled) return;
    
    //FIX #169: Receipt custody packets no longer pass through this path (handled via dedicated variant)
    // Milestones remain limited to DATA packets only.
    
    // Global milestone rate limit (watchdog protection)
    // Even with per-source limits, multiple sources can create burst
    static uint32_t lastGlobalMilestoneMs = 0;
    static const uint32_t MIN_GLOBAL_MILESTONE_INTERVAL_MS = 2000; // Max 1 milestone per 2s globally
    
    if (!configMilestonesEnabled || !shouldEmitMilestone(data.orig_from, data.orig_to)) {
        return;
    }
    
    uint32_t nowMs = millis();
    if (nowMs - lastGlobalMilestoneMs < MIN_GLOBAL_MILESTONE_INTERVAL_MS) {
        LOG_DEBUG("DTN: Milestone suppressed - global rate limit (last %u ms ago)", 
                 (unsigned)(nowMs - lastGlobalMilestoneMs));
        return;
    }
    
    // Early return: channel too busy
    if (airTime && airTime->channelUtilizationPercent() > configMilestoneChUtilMaxPercent) {
        return;
    }
    
    // Early return: recently tombstoned (avoid spam)
    auto itTs = tombstoneUntilMs.find(data.orig_id);
    if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
        return;
    }
    
    // Early return: too far from source and destination
    uint8_t ringToSrc = getHopsAway(data.orig_from);
    uint8_t ringToDst = getHopsAway(data.orig_to);
    uint8_t minRing = (ringToSrc != 255 && ringToDst != 255) ? std::min(ringToSrc, ringToDst) :
                      (ringToSrc != 255) ? ringToSrc :
                      (ringToDst != 255) ? ringToDst : 255;
    if (minRing != 255 && minRing > configMilestoneMaxRing) {
        return;
    }
    
    // Early return: we already have this pending locally
    auto it = pendingById.find(data.orig_id);
    if (it != pendingById.end()) {
        return;
    }
    
    // Rate limiting: per-source minimum interval
    auto itLast = lastProgressEmitMsBySource.find(data.orig_from);
    if (itLast != lastProgressEmitMsBySource.end()) {
        if (millis() - itLast->second < configOriginProgressMinIntervalMs) {
            return;
        }
    }
    
    // All checks passed - emit milestone
    uint32_t via = nodeDB->getNodeNum() & 0xFFu;
    emitReceipt(data.orig_from, data.orig_id,
               meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, via);
    ctrMilestonesSent++;
    lastProgressEmitMsBySource[data.orig_from] = nowMs;
    lastGlobalMilestoneMs = nowMs; // Update global rate limiter
    
    // FIX #65: DON'T set tombstone for PROGRESSED milestones!
    // PROBLEM: Tombstone blocks handleData() → deliverLocal() NEVER called → user NEVER gets message!
    // SOLUTION: PROGRESSED = hop-by-hop ACK (intermediate confirmation), NOT final delivery!
    //           Only DELIVERED receipts should set tombstone (already fixed in FIX #61)
    // NOTE: Rate limiting (lastProgressEmitMsBySource) already prevents spam
    // if (configTombstoneMs) {
    //     tombstoneUntilMs[data.orig_id] = millis() + configTombstoneMs;  ← REMOVED!
    // }
}
// Purpose: process foreign (non-DTN) direct messages for potential DTN capture
// Returns: true if packet was consumed (e.g., duplicate detected)
bool DtnOverlayModule::processForeignDMCapture(const meshtastic_MeshPacket &mp)
{
    // Non-DTN packet: optionally capture foreign DM into overlay
    // Policy: by default do NOT capture foreign unicasts to avoid recursive wrapping in mixed meshes
    if (isFromUs(&mp) || mp.to == nodeDB->getNodeNum()) {
        return false; // Not a foreign packet
    }
    
    bool isDM = (mp.to != NODENUM_BROADCAST && mp.to != NODENUM_BROADCAST_NO_LORA);
    if (!isDM) {
        return false; // Not a direct message
    }
    
    // If we've just delivered this id via DTN locally, drop late-arriving native duplicates
    auto itDeliveredTs = tombstoneUntilMs.find(mp.id);
    if (itDeliveredTs != tombstoneUntilMs.end() && millis() < itDeliveredTs->second) {
        return false;
    }
    
    // Capture-time gating: ignore far/unknown unicasts to avoid ballooning pending
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
    
    //Calculate TTL for foreign capture
    uint32_t ttlMinutes = (configTtlMinutes ? configTtlMinutes : 5);
    
    if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
        // Policy: by default do NOT capture foreign encrypted unicasts to avoid DTN-on-DTN in mixed meshes
        if (!configCaptureForeignEncrypted) return false;
        
        // Tombstone check: avoid rapid re-enqueue of same orig_id
        auto itTs = tombstoneUntilMs.find(mp.id);
        if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) return false;
        
        enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                            ttlMinutes,
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
        if (!configCaptureForeignText) return false;
        
        auto itTs = tombstoneUntilMs.find(mp.id);
        if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) return false;
        
        enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                            ttlMinutes,
                            false, mp.decoded.payload.bytes, mp.decoded.payload.size, true /*allow fallback*/);
        auto it = pendingById.find(mp.id);
        if (it != pendingById.end() && configGraceAckMs && it->second.tries == 0) {
            uint32_t t = millis() + configGraceAckMs + (uint32_t)random(250);
            if (it->second.nextAttemptMs < t) it->second.nextAttemptMs = t;
        }
    } else if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP && mp.decoded.request_id) {
        // Native ACK/NAK: cancel pending for this orig DM id and mark destination as stock for a while
        pendingById.erase(mp.decoded.request_id);
        if (configTombstoneMs) tombstoneUntilMs[mp.decoded.request_id] = millis() + configTombstoneMs;
        stockKnownMs[mp.to] = millis();
    }
    
    return false; // Do not consume non-DTN packets
}
// Purpose: observe telemetry/nodeinfo packets to opportunistically probe for FW+ capability
// Effect: sends lightweight probes to unknown nodes at ≥2 hops distance
void DtnOverlayModule::processTelemetryProbe(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return;
    }
    
    // Only if enabled and we don't already know this origin as FW+
    if (!configTelemetryProbeEnabled || isFwplus(getFrom(&mp))) {
        return;
    }
    
    bool isTelemetry = (mp.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP);
    bool isNodeInfo = (mp.decoded.portnum == meshtastic_PortNum_NODEINFO_APP);
    
    if (!isTelemetry && !isNodeInfo) {
        return;
    }
    
    NodeNum origin = getFrom(&mp);
    uint8_t hops = getHopsAway(origin);
    
    if (hops == 255 || hops < configTelemetryProbeMinRing) {
        return;
    }
    
    // Global rate limiter
    if (isGlobalProbeCooldownActive()) {
        LOG_DEBUG("DTN: Telemetry probe blocked by global rate limiter (need %u sec)", 
                 getGlobalProbeCooldownRemainingSec());
        return;
    }
    
    // Only probe live nodes (seen within 24h)
    if (!isNodeAlive(origin)) {
        LOG_DEBUG("DTN: Telemetry probe blocked - node 0x%x is stale/dead (last_heard=%u sec ago)", 
                 (unsigned)origin, getNodeAgeSec(origin));
        return;
    }
    
    uint32_t nowMs = millis();
    auto it = lastTelemetryProbeToNodeMs.find(origin);
    
    if (it != lastTelemetryProbeToNodeMs.end() && (nowMs - it->second) < configTelemetryProbeCooldownMs) {
        return; // Per-node cooldown active
    }
    
    if (airTime && !airTime->isTxAllowedChannelUtil(true)) {
        return; // Channel too busy
    }
    
    // Send minimal FW+ probe (receipt PROGRESSED, reason=0) to origin
    maybeProbeFwplus(origin);
    lastTelemetryProbeToNodeMs[origin] = nowMs;
    updateGlobalProbeTimestamp();
}
// Purpose: handle incoming FW+ DTN protobuf packets (DATA/RECEIPT) and conservative foreign capture.
// Returns: true if the packet was consumed by DTN; false to let normal processing continue.
bool DtnOverlayModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg)
{
    // Check for duplicate native TEXT delivered via DTN
    if (checkAndSuppressDuplicateNativeText(mp)) {
        return true;
    }

    // Handle LOCAL packets (our own beacons) - just log and return false to let them route
    if (mp.from == RX_SRC_LOCAL) {
        LOG_DEBUG("DTN: Ignoring LOCAL packet (our own beacon)");
        return false; // Let it route normally
    }
    
    // Handle DTN DATA packets
    if (msg && msg->which_variant == meshtastic_FwplusDtn_data_tag) {
        // Check if this is a broadcast or unicast
        bool isBroadcast = (mp.to == NODENUM_BROADCAST || mp.to == NODENUM_BROADCAST_NO_LORA);
        bool isForUs = (mp.to == nodeDB->getNodeNum());
        
        // Check if the DATA payload is destined for us
        bool dataDestIsUs = (msg->variant.data.orig_to == nodeDB->getNodeNum());
        
        // If DTN disabled: reject intermediate custody, always deliver if destination
        // This prevents "black hole" where disabled node is discovered as FW+ but never forwards
        if (!configEnabled) {
            if (dataDestIsUs) {
                // We are the final destination - ALWAYS deliver even if DTN disabled
                LOG_INFO("DTN rx DATA id=0x%x (DESTINATION) - delivering locally (DTN disabled)", 
                         msg->variant.data.orig_id);
                deliverLocal(msg->variant.data);
                
                // Send native ACK for app UX (but NO DTN RECEIPT - we're not advertising DTN capability)
                routingModule->sendAckNak(meshtastic_Routing_Error_NONE, msg->variant.data.orig_from, 
                                         msg->variant.data.orig_id, msg->variant.data.channel, 0);
                return true; // Consume packet
            } else {
                // Intermediate custody - REJECT (DTN disabled, don't advertise as relay)
                LOG_INFO("DTN rx DATA id=0x%x - DTN disabled, rejecting intermediate custody (dest=0x%x)", 
                         msg->variant.data.orig_id, (unsigned)msg->variant.data.orig_to);
                return false; // Let Router handle as normal packet (will relay or drop)
            }
        }
        
        //fw+
        // FIX #98: Mark sender as FW+ ALWAYS when receiving DTN DATA (including overhearing!)
        // PROBLEM: Node 13 receives DTN DATA from Node 12 (overhearing, mp.to=0x16)
        //          markFwplusSeen() was ONLY called for isBroadcast||isForUs||dataDestIsUs
        //          Result: Node 12 NOT in fwplusVersionByNode → NOT in progressive relay candidates!
        // SOLUTION: Always markFwplusSeen for DTN DATA (even overhearing) - sender is clearly FW+
        //fw+
        // markFwplusSeen() updates only fwplusSeenMs timestamp
        //           But chooseProgressiveRelay() checks fwplusVersionByNode!
        //           We MUST update BOTH maps for overhearing nodes
        certifyFwplus(getFrom(&mp), "rx-data");
        
        // FIX #80: Log custody chain for route debugging
        std::ostringstream pathOss;
        for (pb_size_t i = 0; i < msg->variant.data.custody_path_count; ++i) {
            if (i > 0) pathOss << "->";
            pathOss << "0x" << std::hex << (unsigned)msg->variant.data.custody_path[i];
        }
        
        LOG_INFO("DTN rx DATA id=0x%x from=0x%x to=0x%x (mp.to=0x%x) enc=%d ttl_ms=%u chain=[%s]", msg->variant.data.orig_id,
                 (unsigned)getFrom(&mp), (unsigned)msg->variant.data.orig_to, (unsigned)mp.to,
                 (int)msg->variant.data.is_encrypted, (unsigned)msg->variant.data.ttl_remaining_ms, pathOss.str().c_str());
        
        
        // FIX #174: Feed DTN custody chains to DV-ETX routing table (passive learning)
        // FIX: Check router type before static_cast to prevent SEGFAULT!
        // PROBLEM: static_cast<NextHopRouter*>(router) WITHOUT type check → crash if router is FloodingRouter
        // ROOT CAUSE: Node1/11/13 all crashed with chain=[0x16->xxx] - segfault in learnFromDtnCustodyPath()
        //             Router in simulator might be FloodingRouter, not NextHopRouter!
        // SOLUTION: Use RTTI-free helper Router::asNextHopRouter() before calling NextHopRouter methods
        // IMPACT: All custody packets with chain_len >= 2 caused instant crash!
        LOG_DEBUG("DTN: About to process custody chain (chain_len=%u, router=%p)", 
                 (unsigned)msg->variant.data.custody_path_count, (void*)router);
        if (msg->variant.data.custody_path_count >= 2 && router) {
            // Check if router is actually NextHopRouter before calling its methods
            NextHopRouter *nh = router->asNextHopRouter();
            LOG_DEBUG("DTN: asNextHopRouter result: nh=%p (NULL means FloodingRouter)", (void*)nh);
            if (nh) {
                // Convert uint8_t custody_path[] to uint32_t path[] for processPathAndLearn()
                // Max custody_path size is 8 (from protobuf definition)
                uint32_t fullPath[8];
                pb_size_t validCount = 0;
                
                for (pb_size_t i = 0; i < msg->variant.data.custody_path_count && i < 8; ++i) {
                    uint8_t lowByte = msg->variant.data.custody_path[i];
                    // Find full NodeNum matching the low byte
                    NodeNum fullNum = 0;
                    int totalNodes = nodeDB->getNumMeshNodes();
                    for (int j = 0; j < totalNodes; ++j) {
                        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(j);
                        if (!ni) continue;
                        if ((ni->num & 0xFF) == lowByte) {
                            fullNum = ni->num;
                            break;
                        }
                    }
                    if (fullNum != 0) {
                        fullPath[validCount++] = fullNum;
                    }
                }
                
                // Feed to NextHopRouter for passive learning
                if (validCount >= 2) {
                    LOG_DEBUG("DTN: About to call learnFromDtnCustodyPath (validCount=%u)", (unsigned)validCount);
                    nh->learnFromDtnCustodyPath(fullPath, validCount);
                    LOG_INFO("DTN: Fed custody chain to DV-ETX routing table (chain_len=%u)", validCount);
                }
            } else {
                LOG_DEBUG("DTN: Router is not NextHopRouter, skipping custody chain learning (chain_len=%u)", 
                         (unsigned)msg->variant.data.custody_path_count);
            }
        }
        LOG_DEBUG("DTN: Custody chain processing complete, about to call processMilestoneEmission");
        
        if (isBroadcast || isForUs || dataDestIsUs) {
            // Process milestone emission (sparse PROGRESSED receipts)
            LOG_DEBUG("DTN: Calling processMilestoneEmission");
            processMilestoneEmission(mp, msg->variant.data);
            
            // Handle the DATA (will check internally if dest == us for delivery)
            LOG_DEBUG("DTN: About to call handleData (dest=0x%x)", (unsigned)msg->variant.data.orig_to);
            bool isCustodyReceiptForUs = false;
            handleData(mp, msg->variant.data);
            
            //fw+ Removed PhoneAPI sniffer forwarding to avoid duplicate UI entries
            return true; // Consume - we'll relay via DTN custody
        } else {
            // DATA packet for someone else (mp.to != us)
            // Only take custody if packet sent TO US (unicast handoff)
            // PROBLEM: Overhearing DATA unicast to OTHER nodes causes us to enqueue as pending
            //          Node13 hears "mp.to=0x1b" packets and enqueues them (wrong relay!)
            // SOLUTION: Only process as relay if mp.to == us (custody transfer)
            //           If mp.to != us, let Router handle as normal (we're just overhearing)
            bool isCustodyTransferToUs = (mp.to == nodeDB->getNodeNum());
            
            if (isCustodyTransferToUs) {
                LOG_DEBUG("DTN rx DATA (custody transfer to us, will relay) orig_to=0x%x mp.to=0x%x", 
                         (unsigned)msg->variant.data.orig_to, (unsigned)mp.to);
                
                // Process as intermediate relay (take custody)
                processMilestoneEmission(mp, msg->variant.data);
                handleData(mp, msg->variant.data);
                
                //fw+ Removed PhoneAPI sniffer forwarding to avoid duplicate UI entries
                return true; // Consume - we'll relay via DTN custody
            } else {
                LOG_DEBUG("DTN rx DATA (overhearing, not custody transfer) orig_to=0x%x mp.to=0x%x - let Router handle", 
                         (unsigned)msg->variant.data.orig_to, (unsigned)mp.to);
                return false; // Let Router handle as normal packet (we're just overhearing)
            }
        }
    }
    
    // Handle DTN RECEIPT packets
    if (msg && msg->which_variant == meshtastic_FwplusDtn_receipt_tag) {
        // Check if this packet is FOR US or is a broadcast (beacon)
        bool isForUs = (mp.to == nodeDB->getNodeNum());
        bool isBroadcast = (mp.to == NODENUM_BROADCAST || mp.to == NODENUM_BROADCAST_NO_LORA);
        
        // If DTN disabled: still observe FW+ nodes (passive discovery)
        // but we DON'T advertise ourselves as FW+ capable (skip markFwplusSeen for self)
        // We DO process RECEIPTs for us (delivery confirmations) even if disabled
        if (!configEnabled) {
            if (isForUs) {
                // RECEIPT for us - process even if DTN disabled (e.g., reply to probe we sent before disabling)
                LOG_INFO("DTN rx RECEIPT id=0x%x status=%u from=0x%x (FOR US, DTN disabled)", 
                         msg->variant.receipt.orig_id, (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp));
                // Observe sender's FW+ capability (passive learning)
                if (msg->variant.receipt.reason > 0 && isVersionReason(msg->variant.receipt.reason)) {
                    NodeNum origin = getFrom(&mp);
                    uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                    recordFwplusVersion(origin, ver);
                } else {
                    LOG_DEBUG("DTN: Ignoring non-version reason value %u in RECEIPT from 0x%x",
                             msg->variant.receipt.reason, getFrom(&mp));
                }
                // Don't call handleReceipt() - we have no pending DTN packets when disabled
                return true; // Consume - this is for us
            } else {
                // Broadcast or transit RECEIPT - observe but don't mark ourselves as FW+
                if (isBroadcast || msg->variant.receipt.reason > 0) {
                    NodeNum origin = getFrom(&mp);
                    uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                    if (ver > 0 && isVersionReason(msg->variant.receipt.reason)) {
                        LOG_DEBUG("DTN: Passive discovery - node 0x%x is FW+ v%u (DTN disabled)", 
                                 (unsigned)origin, (unsigned)ver);
                        recordFwplusVersion(origin, ver);
                    } else {
                        LOG_DEBUG("DTN: Ignoring non-version reason value %u in RECEIPT from 0x%x",
                                 msg->variant.receipt.reason, origin);
                    }
                }
                return false; // Don't consume - let it propagate/relay
            }
        }
        
        // DTN enabled - mark sender as FW+ capable and process normally
        certifyFwplus(getFrom(&mp), "rx-receipt");
        
        if (isBroadcast) {
            // Broadcast RECEIPT (beacon) - process for discovery but DON'T consume (let it propagate)
            LOG_INFO("DTN rx BEACON id=0x%x status=%u from=0x%x", msg->variant.receipt.orig_id,
                     (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp));
            handleReceipt(mp, msg->variant.receipt);
            return false; // Don't consume - let it propagate through mesh
        } else if (isForUs) {
            // Unicast RECEIPT for us (probe reply, delivery receipt) - process and consume
            LOG_INFO("DTN rx RECEIPT id=0x%x status=%u from=0x%x to=0x%x (FOR US)", msg->variant.receipt.orig_id,
                     (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp), (unsigned)mp.to);
            handleReceipt(mp, msg->variant.receipt);
            
            //fw+ Removed PhoneAPI sniffer forwarding to avoid duplicate UI entries
            return true; // Consume - this is for us
        } else {
            // Unicast RECEIPT for someone else - observe for discovery but DON'T consume (let Router relay)
            LOG_DEBUG("DTN rx RECEIPT id=0x%x from=0x%x to=0x%x (relay - not for us)", 
                     msg->variant.receipt.orig_id, (unsigned)getFrom(&mp), (unsigned)mp.to);
            // Still process for discovery (already called markFwplusSeen above)
            // But check for version advertisement
            if (msg->variant.receipt.reason > 0 && isVersionReason(msg->variant.receipt.reason)) {
                LOG_DEBUG("DTN: Observing version advertisement in transit packet (reason=0x%x)", (unsigned)msg->variant.receipt.reason);
                NodeNum origin = getFrom(&mp);
                uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                recordFwplusVersion(origin, ver);
            } else if (msg->variant.receipt.reason > 0) {
                LOG_DEBUG("DTN: Ignoring non-version reason value %u in transit RECEIPT from 0x%x",
                         msg->variant.receipt.reason, getFrom(&mp));
            }
            return false; // Don't consume - let Router relay to actual destination
        }
    }
    
    // Process foreign DM capture (conservative policy)
    if (processForeignDMCapture(mp)) {
        return true; // Packet consumed
    }
    
    // Process telemetry-triggered FW+ probes
    processTelemetryProbe(mp);
    
    // Observe OnDemand responses for DTN discovery
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.portnum == meshtastic_PortNum_ON_DEMAND_APP) {
        observeOnDemandResponse(mp);
    }
    
    return false; // Do not consume non-DTN packets; allow normal processing
}
//Create DTN envelope from captured DM and schedule forwarding
// Uses relative TTL (ttl_remaining_ms) - works in all scenarios (GPS/no-GPS, mixed network)
bool DtnOverlayModule::enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                                            uint32_t ttlMinutes, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                                            bool allowProxyFallback)
{
    uint8_t plainBuf[DtnOverlayModule::kDtnPayloadBytes] = {0};
    pb_size_t plainSize = size;
    if (plainSize > sizeof(plainBuf)) plainSize = sizeof(plainBuf);
    if (plainSize > 0) memcpy(plainBuf, bytes, plainSize);
    bool originalIsEncrypted = isEncrypted;

    //FIX #126 + FIX #131: Guard against malformed packets (invalid fields cause crashes/duplicates!)
    // PROBLEM: Router occasionally receives malformed packets with to=0x0 or id=0x0
    //          DTN intercepts, calls enqueueFromCaptured(origTo=0x0 or origId=0x0)
    //          sendProxyFallback(0x0) → packetPool.allocCopy() → NULL pointer access → CRASH!
    // ROOT CAUSE: Malformed packet from RF corruption or bug in another node (e.g., FIX #130 before fix)
    // EXAMPLE: ESP32 crash log:
    //          "ERROR | Packet received with to: of 0!"
    //          "DTN capture id=0x0 src=0x1f9ffb04 dst=0x0"
    //          "Guru Meditation Error: Core 1 panic'ed (LoadProhibited)"
    //          EXCVADDR: 0x1f9ffb08 (invalid memory access)
    // FIX #131: Also reject origId=0x0 (causes duplicate delivery after 5min retry)
    // EXAMPLE: Node 13 pending "id=0x0 next=37917 ms" → retry → duplicate "dluga droga do domu"
    // SOLUTION: Reject packets with origId=0, origTo=0, or origFrom=0 (all invalid)
    // BENEFIT: Prevents ESP32 crash AND duplicate delivery from malformed packets
    if (origId == 0 || origTo == 0 || origFrom == 0) {
        LOG_ERROR("DTN FIX #126/#131: Rejecting malformed packet id=0x%x from=0x%x to=0x%x (invalid fields, prevents crash/duplicates!)",
                 origId, origFrom, origTo);
        return false; // Reject invalid packet
    }
    
    //FIX #105b: Tombstone must block ALL local packets (including fallback loop)
    // PROBLEM: isSourceRetry exemption allows fallback DM re-capture → infinite loop!
    //          Fallback DM: sendToMesh() → Router → DTN intercept → enqueueFromCaptured()
    //          → isSourceRetry=true (from=us) → tombstone bypassed → RE-CAPTURED! 😱
    // SOLUTION: Check tombstone for ALL packets (no isSourceRetry exemption)
    //           Source retry is handled by checking if packet exists in pendingById
    auto itTs = tombstoneUntilMs.find(origId);
    if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
        LOG_DEBUG("DTN: Skip re-capture of id=0x%x - tombstone active (prevents fallback loop)", origId);
        return false; // Packet skipped (tombstone active) (OK)
    }
    
    // Check if packet already exists and preserve its TTL
    // PROBLEM: Re-capture with full TTL would reset countdown
    // SOLUTION: Check pendingById and preserve existing TTL if found
    uint32_t preservedTtl = 0;
    bool isSourceRetry = (origFrom == nodeDB->getNodeNum());
    auto itExisting = pendingById.find(origId);
    if (isSourceRetry && itExisting != pendingById.end()) {
        preservedTtl = itExisting->second.data.ttl_remaining_ms;
        LOG_DEBUG("DTN: Source retry for id=0x%x - preserving TTL=%u (was in pending)", 
                 origId, preservedTtl);
    }
    
    //guard: if payload won't fit into FW+ DTN container, skip overlay to avoid corrupting DM
    meshtastic_FwplusDtnData d = meshtastic_FwplusDtnData_init_zero;
    if (size > sizeof(d.payload.bytes)) {
        LOG_WARN("DTN skip too-large DM id=0x%x size=%u limit=%u", origId, (unsigned)size, (unsigned)sizeof(d.payload.bytes));
        return false; // Packet too large
    }

    //guard: cap queue to avoid memory growth/fragmentation
    if (pendingById.size() >= kMaxPendingEntries) {
        LOG_WARN("DTN queue full (%u), drop id=0x%x", (unsigned)pendingById.size(), origId);
        return false; // Queue full
    }

    d.orig_id = origId;
    d.orig_from = origFrom;
    d.orig_to = origTo;
    d.channel = channel;
    //NEW: Relative TTL in milliseconds (simple and universal)
    // If this is a source retry with existing TTL, preserve it (don't reset!)
    if (preservedTtl > 0) {
        d.ttl_remaining_ms = preservedTtl;  // Keep existing TTL
    } else {
        d.ttl_remaining_ms = ttlMinutes * 60 * 1000;  // New packet
    }
    d.is_encrypted = isEncrypted;
    d.allow_proxy_fallback = allowProxyFallback;
    d.use_pki_encryption = false;

    bool pkiWrapped = false;
    uint32_t nowMs = millis();
    bool canAttemptPki = configEnableE2ePki && origFrom == nodeDB->getNodeNum();
    auto retryIt = pkiRetryAfterMs.find(origTo);
    if (retryIt != pkiRetryAfterMs.end()) {
        if (nowMs < retryIt->second) {
            canAttemptPki = false;
        } else {
            pkiRetryAfterMs.erase(retryIt);
        }
    }

    if (canAttemptPki) {
        meshtastic_NodeInfoLite *destInfo = nodeDB->getMeshNode(origTo);
        if (destInfo && destInfo->user.public_key.size == 32) {
            size_t cipherLen = size + MESHTASTIC_PKC_OVERHEAD;
            if (cipherLen <= sizeof(d.payload.bytes)) {
                uint8_t cipherBuf[sizeof(d.payload.bytes)] = {0};
                if (crypto->encryptCurve25519(origTo, nodeDB->getNodeNum(), destInfo->user.public_key, origId,
                                              size, bytes, cipherBuf)) {
                    memcpy(d.payload.bytes, cipherBuf, cipherLen);
                    d.payload.size = cipherLen;
                    d.use_pki_encryption = true;
                    d.is_encrypted = false; // handled via PKI wrapper instead of symmetric payload flag
                    pkiWrapped = true;
                    LOG_INFO("DTN: Applied E2E PKI payload wrapping id=0x%x to=0x%x (cipher=%u bytes)",
                             origId, origTo, (unsigned)cipherLen);
                } else {
                    LOG_WARN("DTN: Failed to encrypt payload via PKI for id=0x%x to=0x%x", origId, origTo);
                }
                memset(cipherBuf, 0, sizeof(cipherBuf));
            } else {
                LOG_WARN("DTN: Skip PKI wrapping id=0x%x - ciphertext would exceed limit (%u > %u)",
                         origId, (unsigned)(size + MESHTASTIC_PKC_OVERHEAD), (unsigned)sizeof(d.payload.bytes));
            }
        } else {
            LOG_DEBUG("DTN: Skip PKI wrapping id=0x%x - destination 0x%x missing public key", origId, origTo);
        }
    }

    if (!pkiWrapped) {
        memcpy(d.payload.bytes, bytes, size);
        d.payload.size = size;
        d.use_pki_encryption = false;
    }
    
    // FIX #80 + #90: Initialize custody_path with source node for global loop detection
    // PROBLEM: Local recentCarriers[3] buffer can't detect loops in long custody chains (A→B→C→D→B)
    // SOLUTION: Track full custody chain in protobuf custody_path field (max 16 hops)
    // BENEFIT: Prevents custody loops, enables RECEIPT reverse routing, provides debugging trace
    // PAYLOAD: Adds ~1 byte per hop (compact encoding using NodeNum & 0xFF)
    // FIX #90: Explicit zeroing of custody_path array to prevent corruption
    // PROBLEM: meshtastic_FwplusDtnData_init_zero may not clear custody_path array (garbage from previous messages)
    //          Result: "Loop detected in NEW entry" for fresh PhoneAPI messages (Test 4b, Repeat 2)
    // SOLUTION: Explicitly zero custody_path before adding source node
    memset(d.custody_path, 0, sizeof(d.custody_path));
    d.custody_path_count = 0;
    if (d.custody_path_count < 16) {
        d.custody_path[d.custody_path_count++] = origFrom & 0xFF; // Source as first custody holder
    }
    
    //Create tombstone to prevent re-capture during Router retransmissions
    // Tombstone duration = TTL (relative, millis-based)
    if (configTombstoneMs && d.ttl_remaining_ms > 0) {
        tombstoneUntilMs[origId] = millis() + d.ttl_remaining_ms;
        LOG_DEBUG("DTN: Created tombstone for id=0x%x (duration=%u ms) to prevent re-capture", 
                 origId, (unsigned)d.ttl_remaining_ms);
    }
    
    LOG_DEBUG("DTN capture id=0x%x src=0x%x dst=0x%x enc=%d ch=%u ttl_ms=%u", origId, (unsigned)origFrom,
              (unsigned)origTo, (int)isEncrypted, (unsigned)channel, (unsigned)d.ttl_remaining_ms);
    scheduleOrUpdate(origId, d);

    auto itPending = pendingById.find(origId);
    if (itPending != pendingById.end()) {
        Pending &pending = itPending->second;
        pending.hasPlainPayload = (plainSize > 0);
        pending.plainPayloadSize = plainSize;
        if (plainSize > 0) memcpy(pending.plainPayload, plainBuf, plainSize);
        pending.usePki = d.use_pki_encryption;
        pending.disablePki = false;
        pending.originalIsEncrypted = originalIsEncrypted;
    }
    
    return true; // Packet was successfully enqueued
}
// Purpose: process received FWPLUS_DTN DATA; deliver locally if destined to us, else schedule and coordinate with peers.
void DtnOverlayModule::handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d)
{
    //fw+
    certifyFwplus(d.orig_from, "data-orig");

    // If we are the destination, deliver locally
    if (d.orig_to == nodeDB->getNodeNum()) {
        auto itDelivered = tombstoneUntilMs.find(d.orig_id);
        if (itDelivered != tombstoneUntilMs.end() && millis() < itDelivered->second) {
            LOG_DEBUG("DTN: Ignoring duplicate delivery for id=0x%x (already delivered)", d.orig_id);
            ctrDuplicatesSuppressed++;
            return;
        }

        bool insertedTombstone = false;
        uint32_t previousExpiry = 0;

        if (configTombstoneMs) {
            auto existing = tombstoneUntilMs.find(d.orig_id);
            uint32_t minTombstone = (configRetryBackoffMs * 3);
            uint32_t tombstoneDuration = (d.ttl_remaining_ms > minTombstone) ? d.ttl_remaining_ms : minTombstone;
            uint32_t newExpiry = millis() + tombstoneDuration;

            if (existing == tombstoneUntilMs.end() || newExpiry > existing->second) {
                previousExpiry = (existing == tombstoneUntilMs.end()) ? 0 : existing->second;
                tombstoneUntilMs[d.orig_id] = newExpiry;
                insertedTombstone = true;
                LOG_DEBUG("DTN: Tombstone set/extended for id=0x%x BEFORE deliverLocal() (duration=%u ms, ttl=%u)",
                         d.orig_id, tombstoneDuration, d.ttl_remaining_ms);
            } else {
                LOG_DEBUG("DTN: Keeping existing longer tombstone for id=0x%x BEFORE deliverLocal() (expires in %u ms)",
                         d.orig_id, existing->second - millis());
            }
        }

        bool delivered = deliverLocal(d);
        if (!delivered) {
            if (configTombstoneMs && insertedTombstone) {
                if (previousExpiry == 0) {
                    tombstoneUntilMs.erase(d.orig_id);
                } else {
                    tombstoneUntilMs[d.orig_id] = previousExpiry;
                }
            }
            return;
        }

        ctrDeliveredLocal++;

        uint32_t via = nodeDB->getNodeNum() & 0xFFu;
        LOG_INFO("DTN delivered id=0x%x to=0x%x src=0x%x via=0x%x", d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from, (unsigned)via);
        LOG_INFO("DTN: custody ack id=0x%x from=0x%x to=0x%x",
                 (unsigned)d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from);

        emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED, via,
                    d.custody_path_count, d.custody_path);

        auto itStock = stockKnownMs.find(nodeDB->getNodeNum());
        if (itStock != stockKnownMs.end()) {
            LOG_DEBUG("DTN: Clearing our own stock marking (we just received DTN!)");
            stockKnownMs.erase(itStock);
        }

        pendingById.erase(d.orig_id);

        return;
    }
    //FIX #163: Loop detection via custody_path check (prevent custody loops)
    // PROBLEM: Node3 receives custody packet, forwards it, then receives SAME packet again
    //          (via broadcast fallback or RF overhearing) and re-schedules as NEW pending
    // EXAMPLE: chain=[0x1d->0x1b->0x13->0x1c] received by Node3 (0x13 already in path!)
    //          Node3 doesn't check custody_path → creates NEW pending → LOOP! ❌
    // ROOT CAUSE: 11 pending custody packets on Node3 = same packets looping back!
    //             Result: channel util climbs to 42%, deadlock, messages never delivered
    // SOLUTION: Check if OUR NodeNum already exists in custody_path BEFORE accepting custody
    //           If we're already in the chain, REJECT custody (packet looping back!)
    // BENEFIT: Prevents custody loops, stops pending accumulation, keeps channel clean
    // NOTE: This is CRITICAL for hub nodes which see many custody transfers!
    NodeNum ourNodeNum = nodeDB->getNodeNum();
    //fw+
    for (pb_size_t i = 0; i < d.custody_path_count; ++i) {
        NodeNum hop = resolveLowByteToNodeNum(d.custody_path[i]);
        if (hop != 0) {
            certifyFwplus(hop, "custody-path");
        }
    }
    bool alreadyInCustodyPath = false;
    for (pb_size_t i = 0; i < d.custody_path_count; ++i) {
        if (d.custody_path[i] == ourNodeNum) {
            alreadyInCustodyPath = true;
            break;
        }
    }
    
    if (alreadyInCustodyPath) {
        LOG_WARN("DTN: LOOP DETECTED! id=0x%x custody_path contains 0x%x (us) - REJECTING custody (prevents loop)",
                 d.orig_id, (unsigned)ourNodeNum);
        ctrDuplicatesSuppressed++; // Count as duplicate (it's a loop)

        //Immediate failure receipt so upstream can reroute without long backoff
        emitReceipt(d.orig_from, d.orig_id,
                    meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                    meshtastic_Routing_Error_NO_ROUTE,
                    d.custody_path_count, d.custody_path);

        return; // REJECT custody - packet already passed through us!
    }
    // Otherwise, coordinate with others: if we heard someone else carrying this id, suppress our attempt for a while 
    auto it = pendingById.find(d.orig_id);
    if (it == pendingById.end()) {
        //FIX #170 + #171: Enforce maxActive limit BEFORE accepting custody transfer
        // PROBLEM: Intermediate nodes accept unlimited custody transfers → pending accumulation → packet storm
        //          Example: Node3 has 8 pending (maxActive=3!), channel util >25%, node1 CRASH!
        //          Test: node3.log line 6342-6350 shows 8 pending + Ch. util >25% warnings
        // ROOT CAUSE: handleData() calls scheduleOrUpdate() without checking pendingById.size()
        //             Result: Intermediate nodes become bottlenecks, accumulate packets, crash network
        // EXAMPLE: 4 messages test → Node3 accumulates:
        //          - 0x2fbc9cf6, 0x2fbc9cf8, 0x2541700b (data messages) = 3
        //          - 0xefd7c576, 0xfcd7d9ed, 0x43df0577, 0xf5d7cee8, 0x803e36ed (receipt custody) = 5
        //          = 8 pending → channel util >25% → STORM!
        //
        // FIX #171: Detect receipt custody and use separate limit (or skip custody entirely)
        // PROBLEM: Receipts compete with DATA for custody slots → legitimate messages REJECTED!
        //          Example: User sends 5 messages → 3 accepted, 2 REJECTED (maxActive=3)
        //          But 5/8 pendings are RECEIPTS, not user data! User suffers for receipt overhead!
        // ROOT CAUSE: Receipt custody uses same pending pool as DATA → unfair competition
        // SOLUTION: Detect receipt custody (magic bytes 0xAC 0xDC) and either:
        //           A) Use separate limit (e.g., maxActive for DATA, unlimited for receipts with broadcast fallback)
        //           B) Skip custody for receipts entirely (broadcast via routing table)
        // BENEFIT: User messages prioritized, receipts don't block legitimate traffic
        //
        // Check if this is receipt custody (magic bytes 0xAC 0xDC)
        bool isReceiptCustody = false;

        if (pendingById.size() >= configMaxActiveDm) {
            // DATA message: enforce maxActive limit strictly
            LOG_WARN("DTN: REJECTING DATA custody id=0x%x - pending limit reached (%u/%u), intermediate node overloaded",
                     d.orig_id, (unsigned)pendingById.size(), (unsigned)configMaxActiveDm);
            
            // Emit FAILED receipt to inform sender about overload
            // Sender should try different route or wait for capacity
            // NOTE: Use FAILED status (no REJECTED in proto) with reason code for overload
            emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED, 
                        0x01, // reason: overload (custom reason code)
                        0, nullptr);
            
            ctrDuplicatesSuppressed++; // Count as suppressed (overload protection)
            return; // Don't accept custody - node is overloaded
        } else if (isReceiptCustody && pendingById.size() >= (configMaxActiveDm + 1)) {
            // RECEIPT custody: use relaxed limit (maxActive + 1) to allow SOME receipts while protecting DATA priority
            // Receipts are small, less critical than DATA, but unlimited receipts cause channel congestion
            // Example: 5 receipts @ 02:51:23 → 33% channel_utilization → UNACCEPTABLE!
            // SOLUTION: maxActive=3 for DATA, maxActive=4 for RECEIPT (only +1 buffer)
            LOG_WARN("DTN: REJECTING RECEIPT custody id=0x%x - receipt limit reached (%u/%u), dropping receipt",
                     d.orig_id, (unsigned)pendingById.size(), (unsigned)(configMaxActiveDm + 1));
            
            // Don't emit receipt for rejected receipt (prevents receipt loop!)
            ctrDuplicatesSuppressed++;
            return; // Drop receipt - too many receipts pending
        }
        
        // FIX: Custody Path Coordination - use custody_path for intermediate node ACK
        // PROBLEM: Intermediate nodes (e.g., Node12) hold custody for packet (test212) even after delivery
        //          Receipt returns via reverse path but intermediate nodes don't erase pending
        //          Result: pending accumulates (3, 5, 8+ packets), nodes crash, channel congested
        // EXAMPLE: test212: Node1→Node17→Node13 (delivered!)
        //          Receipt: Node13→Node17→Node1 (delivered!)
        //          BUT Node12 (overhearing) still holds test212 pending! ❌
        // ROOT CAUSE: Intermediate nodes capture foreign carry (broadcast) but never get explicit ACK
        //             Receipt travels via reverse path, doesn't reach "off-path" intermediate nodes
        // SOLUTION: When intermediate node receives receipt custody with custody_path:
        //           1. Decode original_id from receipt payload
        //           2. Check if I have pending for original_id
        //           3. Check custody_path: if ANY node AFTER me in forward chain is in receipt path
        //              → my successor confirmed delivery → ERASE my pending!
        // BENEFIT: Automatic pending cleanup via custody coordination, no crashes, clean channel
        // MECHANISM: Receipt custody_path shows reverse route, compare with our position in forward chain
        //           If receipt came "from ahead", our job is done!
        //Cold start check for intermediate nodes: if we're cold and destination is not FW+, use native DM fallback
        if (isDtnCold() && !isFwplus(d.orig_to) && d.allow_proxy_fallback) {
            LOG_INFO("DTN: Intermediate cold start - using native DM fallback for id=0x%x dest=0x%x", 
                     d.orig_id, (unsigned)d.orig_to);
            
            // NOTE: Aggressive discovery removed - rely on broadcast beacons only
            
            // Create a temporary pending entry just for fallback
            Pending tempPending;
            tempPending.data = d;
            tempPending.tries = 0;
            
            if (sendProxyFallback(d.orig_id, tempPending)) {
                LOG_INFO("DTN: Cold start fallback sent successfully");
                return; // Don't enqueue for DTN processing
            }
        }
        
        // FIX: Receipt Loop Prevention - check custody_path before accepting receipt custody
        // PROBLEM: Receipts loop infinitely through intermediate nodes (e.g., 0x6025253b: chain=[0x1d->0x17->0x16->0x13->0x1f])
        //          Example: Node12 receives receipt, takes custody, forwards it
        //                   Receipt loops back to Node12 → takes custody AGAIN → infinite loop
        // ROOT CAUSE: Intermediate nodes don't check if they're ALREADY in receipt custody_path
        //             Regular DATA: loop check exists (line 1679-1685) but only prevents adding to path
        //             Receipts: need stronger check - REJECT custody if already in path
        // SOLUTION: For receipt custody, check if we're already in custody_path
        //           If YES → RETURN (don't take custody again, receipt already passed through us)
        // BENEFIT: Receipt loops eliminated, pending queues stay clean, no crashes
        if (isReceiptCustody) {
            uint8_t selfByte = nodeDB->getNodeNum() & 0xFF;
            bool alreadyInReceiptPath = false;
            for (pb_size_t i = 0; i < d.custody_path_count; ++i) {
                if (d.custody_path[i] == selfByte) {
                    alreadyInReceiptPath = true;
                    LOG_INFO("DTN: Receipt loop prevention - rejecting receipt custody id=0x%x (we're already in custody_path at pos %u)",
                             d.orig_id, (unsigned)i);
                    break;
                }
            }
            if (alreadyInReceiptPath) {
                // Receipt already passed through us - don't take custody again
                return; // Reject custody to prevent receipt loop
            }
        }
        // First time we see this id (from overlay or capture) → schedule normally
        scheduleOrUpdate(d.orig_id, d);
        // Track who carried this packet for loop detection
        auto &p = pendingById[d.orig_id];
        trackCarrier(p, getFrom(&mp));
        
        //FIX #168: Mark receipt custody to disable progressive relay
        // PROBLEM: Intermediate nodes use progressive relay for receipts → suboptimal paths → loss
        //          Example: Node12 receives receipt custody, progressive relay to Node31 (unreachable)
        // SOLUTION: Mark as receipt custody (already detected above for FIX #171)
        //           Routing logic will use learned chain/routing table instead of progressive relay
        // BENEFIT: Receipts follow better paths, higher DELIVERED receipt success rate
        // NOTE: isReceiptCustody already declared and checked above (FIX #171)
        if (isReceiptCustody) {
            p.isReceiptCustody = true;
            LOG_DEBUG("DTN: Intermediate node detected receipt custody id=0x%x (will use routing table, not progressive relay)",
                     d.orig_id);

            p.hasReceiptReturnChain = false;
            p.receiptReturnLen = 0;
            p.receiptReturnIndex = 0;
            p.receiptSelfIndex = 0;
            p.receiptHopRetries = 0;
            p.receiptHopStartMs = millis();

            //fw+ Decode reverse return chain hints embedded by destination
            if (d.payload.size >= 12) {
                uint8_t encodedLen = d.payload.bytes[11];
                size_t available = d.payload.size - 12;
                if (encodedLen > available) {
                    encodedLen = static_cast<uint8_t>(available);
                }
                if (encodedLen > sizeof(p.receiptReturnChain)) {
                    encodedLen = static_cast<uint8_t>(sizeof(p.receiptReturnChain));
                }

                if (encodedLen > 0) {
                    uint8_t selfByte = nodeDB->getNodeNum() & 0xFF;
                    bool selfFound = false;
                    uint8_t selfIdx = 0;

                    memcpy(p.receiptReturnChain, &d.payload.bytes[12], encodedLen);
                    p.receiptReturnLen = encodedLen;

                    for (uint8_t i = 0; i < encodedLen; ++i) {
                        if (p.receiptReturnChain[i] == selfByte) {
                            selfFound = true;
                            selfIdx = i;
                            break;
                        }
                    }

                    if (selfFound) {
                        p.hasReceiptReturnChain = true;
                        p.receiptSelfIndex = selfIdx;

                        uint8_t targetIdx = selfIdx;
                        if (selfIdx + 1 < encodedLen) {
                            targetIdx = selfIdx + 1; // prefer forward (toward source)
                        } else if (selfIdx > 0) {
                            targetIdx = selfIdx - 1; // otherwise fall back one hop toward destination
                        }

                        NodeNum forcedNext = applyReceiptReturnHop(p, targetIdx);
                        if (forcedNext != 0) {
                            LOG_INFO("DTN: Receipt id=0x%x honoring return chain hop 0x%x (idx=%u/%u)",
                                     d.orig_id, (unsigned)forcedNext,
                                     (unsigned)p.receiptReturnIndex, (unsigned)p.receiptReturnLen);
                        } else {
                            uint8_t hopByte = p.receiptReturnChain[p.receiptReturnIndex];
                            LOG_WARN("DTN: Receipt id=0x%x planned hop 0x%x unavailable - will attempt recovery",
                                     d.orig_id, (unsigned)hopByte);
                        }
                    } else {
                        // We aren't in the chain – ignore hints
                        p.receiptReturnLen = 0;
                    }
                }
            }

            if (!p.hasReceiptReturnChain) {
                //fw+ steer receipt custody along reverse custody_path (legacy fallback)
                uint8_t selfByte = nodeDB->getNodeNum() & 0xFF;
                int selfIndex = -1;
                for (pb_size_t i = 0; i < p.data.custody_path_count; ++i) {
                    if (p.data.custody_path[i] == selfByte) {
                        selfIndex = static_cast<int>(i);
                        break;
                    }
                }

                if (selfIndex > 0) {
                    uint8_t prevHopByte = p.data.custody_path[selfIndex - 1];
                    NodeNum prevHop = resolveNextHopNode(prevHopByte);
                    if (prevHop == 0) {
                        prevHop = static_cast<NodeNum>(prevHopByte);
                    }

                    if (prevHop != nodeDB->getNodeNum()) {
                        bool prevReachable = isNodeReachable(prevHop, configReceiptMaxNodeAgeSec);
                        if (prevReachable) {
                            p.forceEdge = prevHop;
                            LOG_DEBUG("DTN: Receipt custody id=0x%x forcing reverse hop 0x%x (custody_path idx=%d)",
                                      d.orig_id, (unsigned)prevHop, selfIndex - 1);
                            p.receiptHopRetries = 0;
                            p.receiptHopStartMs = millis();
                        } else {
                            LOG_WARN("DTN: Receipt custody id=0x%x reverse hop 0x%x unreachable - falling back to routing",
                                     d.orig_id, (unsigned)prevHop);
                        }
                    }
                }
            }
        }
        
        //FIX #124: Add intermediate node to custody_path to prevent loops
        // PROBLEM: Intermediate nodes don't add themselves to custody_path
        //          Result: custody loop (Node 3 ↔ Node 7 ↔ Node 11 ↔ Node 6) because no loop detection!
        //          Example: chain=[0x11] stays [0x11] forever, no matter how many intermediates relay it
        // ROOT CAUSE: Only source node adds itself in enqueueFromCaptured()
        //             Intermediate nodes receive DATA, create pending, but never extend custody_path
        // SOLUTION: When intermediate node takes custody, add self to custody_path
        //           Progressive relay loop detection checks custody_path (line 3575-3582)
        //           Without this, same nodes are selected repeatedly → ping-pong loop
        // Fixes custody ping-pong loop
        if (p.data.custody_path_count < 16) {
            uint8_t selfByte = nodeDB->getNodeNum() & 0xFF;
            // Check if we're already in the path (shouldn't happen, but safety check)
            bool alreadyInPath = false;
            for (pb_size_t i = 0; i < p.data.custody_path_count; ++i) {
                if (p.data.custody_path[i] == selfByte) {
                    alreadyInPath = true;
                    LOG_WARN("DTN FIX #124: Self already in custody_path for id=0x%x (loop detected!)", d.orig_id);
                    break;
                }
            }
            if (!alreadyInPath) {
                p.data.custody_path[p.data.custody_path_count++] = selfByte;
                LOG_DEBUG("DTN FIX #124: Added self (0x%x) to custody_path for id=0x%x (count=%u)",
                         (unsigned)nodeDB->getNodeNum(), d.orig_id, (unsigned)p.data.custody_path_count);
            }
        } else {
            LOG_WARN("DTN FIX #124: custody_path full (16 hops) for id=0x%x - cannot add self", d.orig_id);
        }
        
        // ADAPTIVE ROUTING: Learn from custody_path in received DATA
        // Intermediate nodes can learn chains from packets they relay
        // Skip learning for source node (already have path logic)
        bool isSource = (d.orig_from == nodeDB->getNodeNum());
        if (!isSource && d.custody_path_count >= 2 && d.orig_to != nodeDB->getNodeNum()) {
            // Extract first hop from custody chain (skip source which is always [0])
            uint8_t firstHopByte = d.custody_path[1]; // Second node in chain
            NodeNum firstHop = 0;
            
            // Find full NodeNum matching the low byte
            int totalNodes = nodeDB->getNumMeshNodes();
            for (int i = 0; i < totalNodes; ++i) {
                meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
                if (!ni) continue;
                if ((ni->num & 0xFF) == firstHopByte && isFwplus(ni->num)) {
                    firstHop = ni->num;
                    break;
                }
            }
            
            if (firstHop != 0 && firstHop != nodeDB->getNodeNum()) {
                // Learn this chain for future use
                SuccessfulChain chain;
                chain.firstHop = firstHop;
                chain.learnedMs = millis();
                chain.chainLength = d.custody_path_count;
                
                learnedChainsByDest[d.orig_to] = chain;
                LOG_DEBUG("DTN ChainCache: Learned from DATA custody_path, first-hop 0x%x for dest 0x%x (chain_len=%u)",
                         (unsigned)firstHop, (unsigned)d.orig_to, (unsigned)chain.chainLength);
            }
        }
        
        // FIX: Only apply suppression if we overheard the DATA (not unicast to us)
        // If mp.to == us, this is a handoff/custody transfer - we MUST act, not suppress!
        // Suppression is only for coordination when multiple nodes overhear broadcast/relay
        bool isUnicastToUs = (mp.to == nodeDB->getNodeNum());
        if (configSuppressMsAfterForeign && getFrom(&mp) != nodeDB->getNodeNum() && !isUnicastToUs) {
            applyForeignCarrySuppression(d.orig_id, p);
        }
    } else {
        // We already have an entry; seeing foreign DATA suggests someone carries it → backoff our schedule
        // Track this carrier for loop detection
        trackCarrier(it->second, getFrom(&mp));
        
        // FIX: Only apply suppression if we overheard (not unicast to us)
        // Unicast to us = handoff/custody transfer, we must act!
        bool isUnicastToUs = (mp.to == nodeDB->getNodeNum());
        if (configSuppressMsAfterForeign && !isUnicastToUs) {
            applyForeignCarrySuppression(d.orig_id, it->second);
            applyNearDestExtraSuppression(it->second, d.orig_to);
        }
    }
}
// Purpose: process received FWPLUS_DTN RECEIPT; clear pending, tombstone and decode special reason codes (e.g., version).
void DtnOverlayModule::handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r)
{
    // Simplest: stop any pending entry
    (void)mp;
    
    if (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED &&
        r.reason == meshtastic_Routing_Error_PKI_FAILED) {
        auto itPendingPki = pendingById.find(r.orig_id);
        if (itPendingPki != pendingById.end()) {
            Pending &pending = itPendingPki->second;
            if (pending.hasPlainPayload && pending.plainPayloadSize <= sizeof(pending.data.payload.bytes)) {
                LOG_WARN("DTN: PKI receipt failure for id=0x%x - retrying without PKI", r.orig_id);
                pending.data.use_pki_encryption = false;
                pending.data.is_encrypted = pending.originalIsEncrypted;
                pending.usePki = false;
                pending.disablePki = true;
                pending.data.payload.size = pending.plainPayloadSize;
                if (pending.plainPayloadSize > 0) {
                    memcpy(pending.data.payload.bytes, pending.plainPayload, pending.plainPayloadSize);
                }
                pending.nextAttemptMs = millis() + 250;
                pending.tries = 0;
                pending.cachedProgressiveRelay = 0;
                pkiRetryAfterMs[pending.data.orig_to] = millis() + configPkiRetryDelayMs;
            } else {
                LOG_WARN("DTN: PKI receipt failure for id=0x%x but no plaintext cache available", r.orig_id);
            }
        }
        return;
    }

    NodeNum sender = getFrom(&mp);
    //fw+
    certifyFwplus(sender, "receipt-from");
    if (r.dest != 0 && (!nodeDB || r.dest != nodeDB->getNodeNum())) {
        certifyFwplus(r.dest, "receipt-dest");
    }
    for (pb_size_t i = 0; i < r.custody_path_count; ++i) {
        NodeNum hop = resolveLowByteToNodeNum((uint8_t)(r.custody_path[i] & 0xFFu));
        if (hop != 0) {
            certifyFwplus(hop, "receipt-path");
        }
    }
    if (mp.to == nodeDB->getNodeNum() && r.dest != nodeDB->getNodeNum() && r.custody_id != 0) {
        uint32_t nowMs = millis();
        auto itTs = tombstoneUntilMs.find(r.custody_id);
        if (itTs != tombstoneUntilMs.end() && nowMs < itTs->second) {
            LOG_DEBUG("DTN: Skip receipt custody id=0x%x - tombstone active", (unsigned)r.custody_id);
            return;
        }

        meshtastic_FwplusDtnData skeleton = meshtastic_FwplusDtnData_init_zero;
        skeleton.orig_id = r.custody_id;
        skeleton.orig_from = sender;
        skeleton.orig_to = r.dest;
        skeleton.channel = 0;
        skeleton.is_encrypted = false;
        skeleton.allow_proxy_fallback = r.allow_proxy_fallback;
        skeleton.payload.size = 0;
        skeleton.ttl_remaining_ms = r.ttl_remaining_ms;
        skeleton.use_pki_encryption = false;
        skeleton.custody_path_count = 0;
        if (r.custody_path_count > 0) {
            pb_size_t copyCount = std::min<pb_size_t>(r.custody_path_count, (pb_size_t)sizeof(skeleton.custody_path));
            skeleton.custody_path_count = copyCount;
            for (pb_size_t i = 0; i < copyCount; ++i) {
                skeleton.custody_path[i] = (uint8_t)(r.custody_path[i] & 0xFF);
            }
        }

        scheduleOrUpdate(r.custody_id, skeleton);

        auto itPendingReceipt = pendingById.find(r.custody_id);
        if (itPendingReceipt == pendingById.end()) {
            LOG_WARN("DTN: Receipt custody id=0x%x dropped (queue full)", (unsigned)r.custody_id);
            return;
        }

        Pending &pending = itPendingReceipt->second;
        pending.isReceiptCustody = true;
        pending.isReceiptSource = false;
        pending.isReceiptVariant = true;
        pending.receiptMsg = r;
        pending.data.orig_id = r.custody_id;
        pending.data.orig_from = nodeDB->getNodeNum();
        pending.data.orig_to = r.dest;
        pending.data.channel = 0;
        pending.data.allow_proxy_fallback = r.allow_proxy_fallback;
        pending.data.ttl_remaining_ms = r.ttl_remaining_ms;
        pending.data.payload.size = 0;
        pending.data.custody_path_count = 0;
        if (r.custody_path_count > 0) {
            pb_size_t copyCount = std::min<pb_size_t>(r.custody_path_count, (pb_size_t)sizeof(pending.data.custody_path));
            pending.data.custody_path_count = copyCount;
            for (pb_size_t i = 0; i < copyCount; ++i) {
                pending.data.custody_path[i] = (uint8_t)(r.custody_path[i] & 0xFF);
            }
        }
        pending.hasPlainPayload = false;
        pending.usePki = false;
        pending.disablePki = false;
        pending.receiptMsg.ttl_remaining_ms = pending.data.ttl_remaining_ms;

        pending.receiptReturnLen = 0;
        pending.hasReceiptReturnChain = false;
        pending.receiptReturnIndex = 0;
        pending.forceEdge = 0;
        if (r.custody_path_count > 0) {
            for (int i = (int)r.custody_path_count - 1; i >= 0 && pending.receiptReturnLen < sizeof(pending.receiptReturnChain); --i) {
                uint8_t hop = (uint8_t)(r.custody_path[i] & 0xFF);
                if (pending.receiptReturnLen > 0 && pending.receiptReturnChain[pending.receiptReturnLen - 1] == hop) {
                    continue;
                }
                pending.receiptReturnChain[pending.receiptReturnLen++] = hop;
            }
            if (pending.receiptReturnLen > 0) {
                pending.hasReceiptReturnChain = true;
                NodeNum forcedNext = resolveNextHopNode(pending.receiptReturnChain[0]);
                if (forcedNext == 0) {
                    forcedNext = (NodeNum)pending.receiptReturnChain[0];
                }
                if (forcedNext != 0 && forcedNext != nodeDB->getNodeNum() &&
                    isNodeReachable(forcedNext, configReceiptMaxNodeAgeSec)) {
                    pending.forceEdge = forcedNext;
                }
            }
        }

        deliveredReceiptIds.insert(r.custody_id);
        LOG_INFO("DTN: Accepted receipt custody id=0x%x status=%u dest=0x%x (from=0x%x)",
                 (unsigned)r.custody_id, (unsigned)r.status, (unsigned)r.dest, (unsigned)sender);
        ctrReceiptsReceived++;
        return;
    }

    //FIX #125a: Receipt priority - ignore lower-priority receipts after final status
    // PROBLEM: Source receives DELIVERED (status=1) via custody from destination
    //          Later, intermediate nodes send EXPIRED (status=3) after their retry timeout
    //          Result: APK shows "Message expired" AFTER showing "Delivered" (confusing UX!)
    // ROOT CAUSE: Intermediate nodes didn't hear DELIVERED receipt (went via reverse custody path)
    //             They continue retry attempts → max_tries → emit EXPIRED → forwarded to source
    // EXAMPLE: Node 1→15 via Node 6→15 (delivery successful, 19:23:36)
    //          Node 11 had pending, didn't hear DELIVERED, retried 3x, sent EXPIRED (19:32:35)
    //          Source got: DELIVERED (3x, 19:24:11) then EXPIRED (4x from nodes 3,6,7,11)
    // SOLUTION: Track final receipt status per message ID (tombstone-like)
    //           Ignore late receipts with lower priority (EXPIRED after DELIVERED)
    // PRIORITY: DELIVERED (1) > FAILED (2) > EXPIRED (3) > PROGRESSED (4, milestone only)
    // BENEFIT: APK never sees contradictory status updates, UX remains clean
    // NOTE: This is an optimization - doesn't affect delivery success, only UX
    static std::unordered_map<uint32_t, meshtastic_FwplusDtnStatus> finalReceiptStatus;
    
    bool notifyLocalPhone = false;
    auto itFinal = finalReceiptStatus.find(r.orig_id);
    if (itFinal != finalReceiptStatus.end()) {
        // We already received a final receipt for this message
        meshtastic_FwplusDtnStatus existingStatus = itFinal->second;
        meshtastic_FwplusDtnStatus newStatus = r.status;
        
        // Priority order: DELIVERED (1) > FAILED (2) > EXPIRED (3)
        // PROGRESSED (4) is not final, never stored in finalReceiptStatus
        bool shouldIgnore = false;
        if (existingStatus == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED) {
            // DELIVERED is highest priority - ignore all other receipts
            shouldIgnore = (newStatus != meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED);
        } else if (existingStatus == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED) {
            // FAILED is 2nd priority - ignore EXPIRED but allow DELIVERED
            shouldIgnore = (newStatus == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED);
        }
        // EXPIRED is lowest priority - accept DELIVERED or FAILED
        
        if (shouldIgnore) {
            LOG_INFO("DTN FIX #125a: Ignoring late receipt id=0x%x status=%u (already have final status=%u, priority preserved)",
                     r.orig_id, (unsigned)newStatus, (unsigned)existingStatus);
            return; // Ignore this receipt
        }
    }
    
    // Record final status for future receipt priority checks
    bool isFinalReceipt = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED ||
                           r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED ||
                           r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED);
    if (isFinalReceipt) {
        auto emplaceResult = finalReceiptStatus.emplace(r.orig_id, r.status);
        auto itInserted = emplaceResult.first;
        bool inserted = emplaceResult.second;
        if (!inserted) {
            if (itInserted->second != r.status) {
                itInserted->second = r.status;
                notifyLocalPhone = true;
            }
        } else {
            notifyLocalPhone = true;
        }
        // Cleanup old entries (keep last 100)
        if (finalReceiptStatus.size() > 100) {
            // Simple pruning: erase first entry (oldest in unordered_map iteration order)
            finalReceiptStatus.erase(finalReceiptStatus.begin());
        }
    }
    
    auto itPending = pendingById.find(r.orig_id);
    auto itReceiptPending = (r.custody_id != 0) ? pendingById.find(r.custody_id) : pendingById.end();

    if (itReceiptPending != pendingById.end() &&
        itReceiptPending->second.isReceiptCustody &&
        itReceiptPending->second.data.orig_from == nodeDB->getNodeNum() &&
        r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED &&
        sender != nodeDB->getNodeNum()) {
        Pending pendingCopy = itReceiptPending->second;
        uint32_t nowMs = millis();

        uint32_t tombstoneDuration = configTombstoneMs;
        if (pendingCopy.data.ttl_remaining_ms && pendingCopy.data.ttl_remaining_ms > tombstoneDuration) {
            tombstoneDuration = pendingCopy.data.ttl_remaining_ms;
        }
        if (tombstoneDuration) {
            tombstoneUntilMs[r.custody_id] = nowMs + tombstoneDuration;
        }

        pendingById.erase(itReceiptPending);
        deliveredReceiptIds.erase(r.custody_id);
        LOG_INFO("DTN: Receipt progress id=0x%x acknowledged by hop 0x%x - silencing further retries",
                 r.orig_id, (unsigned)sender);
        return;
    }

    if (itPending != pendingById.end()) {
        NodeNum dest = itPending->second.data.orig_to;
        
        // Reset counter for DELIVERED or PROGRESSED (both indicate destination is active)
        bool shouldReset = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED ||
                           r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED);
        
        if (shouldReset && isFwplus(dest) && itPending->second.dtnFailedAttempts > 0) {
            LOG_INFO("DTN: Receipt (status=%u) indicates FW+ dest 0x%x is responsive - resetting counter (was %u)",
                     (unsigned)r.status, (unsigned)dest, (unsigned)itPending->second.dtnFailedAttempts);
            itPending->second.dtnFailedAttempts = 0;
            itPending->second.fallbackTriggered = false; // Allow future fallback if needed
        }
        
        // Also clear from stockKnownMs if it was marked as stock due to unresponsiveness
        if (shouldReset) {
            auto itStock = stockKnownMs.find(dest);
            if (itStock != stockKnownMs.end()) {
                LOG_DEBUG("DTN: Clearing stock marking for responsive FW+ dest 0x%x", (unsigned)dest);
                stockKnownMs.erase(itStock);
            }
        }
    }
    
    //==============================================================================
    // ADAPTIVE PATH LEARNING: Update path reliability from RECEIPT
    //==============================================================================
    
    if (itPending != pendingById.end()) {
        auto& history = packetPathHistoryMap[r.orig_id];
        
        if (!history.attemptedHops.empty()) {
            //fw+ PROGRESSED receipts should not poison reliability – wait for final outcome
            bool isDelivered = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED);
            bool isHardFailure =
                (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED ||
                 r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED);

            if (!isDelivered && !isHardFailure) {
                LOG_DEBUG("DTN PathLearn: Receipt status=%u for id=0x%x – deferring path reliability update",
                          (unsigned)r.status, r.orig_id);
            } else {
                bool success = isDelivered;

                for (NodeNum hop : history.attemptedHops) {
                    updatePathReliability(hop, history.destination, success);

                    if (hop == history.attemptedHops.back()) {
                        auto healthIt = linkHealthMap.find(hop);
                        float snr = (healthIt != linkHealthMap.end()) ? healthIt->second.avgSnr : 5.0f;
                        float rssi = (healthIt != linkHealthMap.end()) ? healthIt->second.avgRssi : -80.0f;

                        updateLinkHealth(hop, success, snr, rssi);
                    }
                }

                if (success) {
                    LOG_INFO("DTN PathLearn: Packet id=0x%x SUCCESS via path(s): %s",
                             r.orig_id, formatHopList(history.attemptedHops).c_str());

                    // ADAPTIVE ROUTING: cache successful first-hop for this destination
                    if (!history.attemptedHops.empty() && history.destination != 0) {
                        NodeNum firstHop = history.attemptedHops[0];
                        SuccessfulChain chain;
                        chain.firstHop = firstHop;
                        chain.learnedMs = millis();
                        chain.chainLength = history.attemptedHops.size();

                        learnedChainsByDest[history.destination] = chain;
                        LOG_INFO("DTN ChainCache: Learned successful first-hop 0x%x for dest 0x%x (chain_len=%u)",
                                 (unsigned)firstHop, (unsigned)history.destination, (unsigned)chain.chainLength);
                    }
                } else {
                    LOG_WARN("DTN PathLearn: Packet id=0x%x FAILED via path(s): %s (status=%u)",
                             r.orig_id, formatHopList(history.attemptedHops).c_str(), r.status);
                }

                packetPathHistoryMap.erase(r.orig_id);
            }
        }
    }
    if (itPending != pendingById.end() &&
        r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED &&
        itPending->second.data.orig_from == nodeDB->getNodeNum() &&
        itPending->second.tries == 0 &&
        !hasSufficientRouteConfidence(itPending->second.data.orig_to)) {
        maybeTriggerTraceroute(itPending->second.data.orig_to);
    }
    
    //FIX #102: Convert DTN RECEIPT DELIVERED → native ACK for source node's application
    // PROBLEM: Source node receives DTN RECEIPT but application expects native ACK for UX (✓✓ delivered)
    // SOLUTION: When source receives DELIVERED receipt, send local native ACK to inform application
    // BENEFIT: Application gets standard ACK flow, DTN remains transparent to app layer
    
    // Only erase pending for FINAL statuses (DELIVERED/FAILED/EXPIRED)
    // PROGRESSED (status=4) is milestone, not final delivery - keep pending active!
    bool isFinalStatus = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED ||
                          r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED ||
                          r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED);
    
    if (itPending != pendingById.end() && isFinalStatus) {
        //fw+ Stop injecting synthetic ROUTING_APP ACKs — rely on DTN receipt flow for UX updates
        pendingById.erase(itPending);
        itPending = pendingById.end();
    }

    if (itReceiptPending != pendingById.end() && isFinalReceipt) {
        pendingById.erase(itReceiptPending);
        deliveredReceiptIds.erase(r.custody_id);
    }
    
    // FIX #61: Only create tombstone for DELIVERED receipts, not PROGRESSED!
    // PROBLEM: PROGRESSED receipt (status=4) created tombstone, blocking actual delivery
    // EXAMPLE: Test 2 (A→D via B):
    //   - Node D receives PROGRESSED receipt from B (status=4) → tombstone set (FAIL)
    //   - Node D receives DTN DATA from B → "Ignoring duplicate (already delivered)" (FAIL)
    //   - User NEVER sees message! (FAIL)
    // SOLUTION: Tombstone ONLY for DELIVERED (status=1), not for PROGRESSED (status=4)
    // PROGRESSED = hop-by-hop ACK (intermediate confirmation), not final delivery!
    //fw+
    // FIX #90: Prevent shorter tombstone from overwriting longer one
    // PROBLEM: Capture creates 30min tombstone, then RECEIPT overwrites with 30s tombstone
    //          After 30s expires, source retry re-captures → duplicate delivery storm!
    // SOLUTION: Only update tombstone if new expiry is LONGER than existing
    if (configTombstoneMs && r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED) {
        auto existing = tombstoneUntilMs.find(r.orig_id);
        uint32_t newExpiry = millis() + configTombstoneMs;
        
        if (existing == tombstoneUntilMs.end() || newExpiry > existing->second) {
            tombstoneUntilMs[r.orig_id] = newExpiry;
            LOG_DEBUG("DTN: Tombstone set/extended for delivered id=0x%x (duration=%u ms)", r.orig_id, configTombstoneMs);
        } else {
            LOG_DEBUG("DTN: Keeping existing longer tombstone for id=0x%x (current expires in %u ms)", 
                     r.orig_id, existing->second - millis());
        }
    }
    
    ctrReceiptsReceived++;

    //Notify local PhoneAPI about new final status without forwarding binary payloads
    if (notifyLocalPhone && mp.to == nodeDB->getNodeNum()) {
        if (service) {
            meshtastic_MeshPacket *copyForPhone = packetPool.allocCopy(mp);
            if (copyForPhone) {
                service->sendPacketToPhoneRaw(copyForPhone);
                LOG_INFO("DTN: Local final receipt id=0x%x status=%u reason=0x%x forwarded to PhoneAPI",
                         (unsigned)r.orig_id, (unsigned)r.status, (unsigned)r.reason);
            } else {
                LOG_WARN("DTN: Failed to forward receipt id=0x%x to PhoneAPI - packetPool exhausted", (unsigned)r.orig_id);
            }
        }
    }

    // Interpret version ads, exclude PROGRESSED with low reason values
    // PROBLEM: PROGRESSED milestone receipts use reason=milestone_node (e.g. 0x17=23 decimal)
    //          This was interpreted as ver=23, causing version downgrade!
    // SOLUTION: Distinguish version advertisements from milestone nodes:
    //           - Version: reason >= 45 (FW_PLUS_VERSION minimum valid version)
    //           - Milestone: reason < 45 (NodeNum in mesh, typically 16-33 range)
    // Convention: reason field is DUAL-PURPOSE:
    //             - For beacons (id=0): reason = FW_PLUS_VERSION (high value, e.g. 61)
    //             - For milestones (id!=0): reason = milestone_node (low value, e.g. 0x17=23)
    LOG_DEBUG("DTN: Processing receipt reason=0x%x status=%u orig_id=0x%x", 
             (unsigned)r.reason, (unsigned)r.status, (unsigned)r.orig_id);
    
    bool isBeacon = (r.orig_id == 0);
    bool reasonLooksLikeVersion = (r.reason >= 81); // FW_PLUS_VERSION minimum
    
    // Version advertisement: reason >= 45 (valid FW+ version), regardless of status
    if (r.reason > 0 && reasonLooksLikeVersion) {
        LOG_INFO("DTN: Version advertisement detected, reason=0x%x (ver=%u, beacon=%d)", 
                 (unsigned)r.reason, (unsigned)r.reason, (int)isBeacon);
        NodeNum origin = getFrom(&mp);
        bool hadVer = (fwplusVersionByNode.find(origin) != fwplusVersionByNode.end());
        uint16_t ver = (uint16_t)(r.reason & 0xFFFFu);
        recordFwplusVersion(origin, ver);
        
        //FIX #123: Record relay_node as direction hint for beacons
        // PROBLEM: Progressive relay selects random FW+ nodes without direction info
        //          Node 12 sends to Node 6 instead of Node 13 (direct neighbor!)
        // OBSERVATION: Broadcast beacons carry relay_node = last hop before us
        //              Example: Node 13 → Node 12 → Node 11, Node 11 sees relay_node=0x1c (Node 12)
        // SOLUTION: Record relay_node as "direction hint" - this node is closer to beacon source
        // BENEFIT: Progressive relay prioritizes nodes in correct direction (self-learning topology)
        // NOTE: Only for beacons (orig_id=0), not for other receipts
        if (isBeacon && mp.relay_node != 0) {
            NodeNum relayNode = (NodeNum)mp.relay_node; // Last hop before us (8-bit to 32-bit)
            // Only record if relay is different from us (exclude self-originated beacons)
            if (relayNode != nodeDB->getNodeNum()) {
                relayHintsByDest[origin] = relayNode;
                LOG_INFO("DTN: Direction hint - dest 0x%x reachable via relay 0x%x", 
                        (unsigned)origin, (unsigned)relayNode);
            }
        }

        //Random delayed hello-back (prevents RF collision storm)
        // Schedule hello-back with random 1-60s delay to spread responses over time
        // Only for NEW discoveries (first time seeing this FW+ node)
        if (!hadVer && ver > 0) {
            LOG_INFO("DTN: New FW+ node 0x%x discovered with version %u - checking if response needed",
                     (unsigned)origin, (unsigned)ver);

            //FIX #176: Nodes with DTN disabled must not send hello-back responses
            // PROBLEM: Node2 has DTN disabled but still sends hello-back with ver=75
            //          Result: Other nodes think Node2 has DTN, send custody transfers → ignored → packets lost!
            // ROOT CAUSE: hello-back logic checks configHelloBackEnabled but NOT configEnabled
            //             Mixed network nodes (DTN disabled) shouldn't advertise DTN capability
            // SOLUTION: Skip hello-back if DTN is disabled (configEnabled=false)
            // BENEFIT: Accurate DTN discovery, no false positives, custody transfers only to DTN nodes
            // Schedule delayed hello-back response with random delay
            if (configEnabled && configHelloBackEnabled) {
                // Global rate limiter
                if (isGlobalProbeCooldownActive()) {
                    LOG_DEBUG("DTN: Hello-back blocked by global rate limiter (need %u sec)",
                             getGlobalProbeCooldownRemainingSec());
                } else {
                    uint32_t nowMs = millis();
                    auto it = lastHelloBackToNodeMs.find(origin);
                    if (it == lastHelloBackToNodeMs.end() || (nowMs - it->second) >= 10000) { // 10s cooldown for new discoveries
                        // Check if already scheduled
                        auto itScheduled = scheduledHelloBackMs.find(origin);
                        if (itScheduled == scheduledHelloBackMs.end()) {
                            // Random delay: 1-60 seconds (spreads responses to prevent RF collision)
                            uint32_t randomDelayMs = 1000 + random(59000); // 1-60s
                            uint32_t scheduledTime = nowMs + randomDelayMs;
                            scheduledHelloBackMs[origin] = scheduledTime;
                            LOG_INFO("DTN: Scheduled hello-back to 0x%x in %u sec (random delay)", 
                                     (unsigned)origin, randomDelayMs/1000);
                        } else {
                            LOG_DEBUG("DTN: Hello-back to 0x%x already scheduled at %u ms", 
                                     (unsigned)origin, itScheduled->second);
                        }
                    }
                }
            }
        }
        // Optional hello-back: unicast our version to origin (allow periodic responses for FW+DTN discovery)
        LOG_DEBUG("DTN: Version advertisement from origin=0x%x ver=%u hadVer=%d", (unsigned)origin, (unsigned)ver, (int)hadVer);
        //FIX #176: Also check configEnabled for periodic hello-back (same as above)
        if (configEnabled && configHelloBackEnabled) {
            // reply to nodes up to 3 hops away (FW+DTN is alternative software)
            uint8_t hops = getHopsAway(origin);
            LOG_DEBUG("DTN: Hello-back check: hops=%u maxRing=%u", (unsigned)hops, (unsigned)configHelloBackMaxRing);
            if (hops != 255 && hops <= configHelloBackMaxRing) {
                // per-origin rate limit (more frequent for known FW+ nodes)
                uint32_t nowMs = millis();
                auto it = lastHelloBackToNodeMs.find(origin);
                uint32_t requiredInterval = hadVer ? (configHelloBackMinIntervalMs / 2) : configHelloBackMinIntervalMs; // 30min for known, 1h for new
                bool rateOk = (it == lastHelloBackToNodeMs.end() || (nowMs - it->second) >= requiredInterval);
                LOG_DEBUG("DTN: Rate check: lastTx=%u required=%u rateOk=%d", 
                         it == lastHelloBackToNodeMs.end() ? 0 : (unsigned)(nowMs - it->second), 
                         (unsigned)requiredInterval, (int)rateOk);
                if (rateOk) {
                    // Global rate limiter
                    if (isGlobalProbeCooldownActive()) {
                        LOG_DEBUG("DTN: Hello-back blocked by global rate limiter (need %u sec)",
                                 getGlobalProbeCooldownRemainingSec());
                    } else {
                        // channel utilization gate
                        bool channelOk = !(airTime && !airTime->isTxAllowedChannelUtil(true));
                        LOG_DEBUG("DTN: Channel check: channelOk=%d", (int)channelOk);
                        if (channelOk) {
                            // reason carries version; use full FW_PLUS_VERSION (lower 16 bits are used on RX)
                            uint32_t reason = (uint32_t)FW_PLUS_VERSION;
                            emitReceipt(origin, 0, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, reason);
                            lastHelloBackToNodeMs[origin] = nowMs;
                            updateGlobalProbeTimestamp();
                            LOG_INFO("DTN: Hello-back sent to origin=0x%x (discovered FW+ nodes: %u)", 
                                     (unsigned)origin, (unsigned)fwplusVersionByNode.size());
                        } else {
                            LOG_DEBUG("DTN: Channel busy, hello-back blocked");
                        }
                    }
                } else {
                    LOG_DEBUG("DTN: Rate limited, hello-back blocked");
                }
            } else {
                LOG_DEBUG("DTN: Origin too far (hops=%u > %u), hello-back blocked", (unsigned)hops, (unsigned)configHelloBackMaxRing);
            }
        } else {
            LOG_DEBUG("DTN: Hello-back disabled");
        }
    } else {
        LOG_DEBUG("DTN: Not a version advertisement (reason=0x%x)", (unsigned)r.reason);
    }
    // If we get a native ROUTING_APP ACK/NAK echo for our original id, mark destination as stock for some time
    if (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED) {
        // DELIVERED came from dest (FW+), do nothing
    }
}

// Purpose: track carrier in loop detection buffer and check for loops
// Returns: true if this carrier creates a loop (already in recent history)
bool DtnOverlayModule::isCarrierLoop(Pending &p, NodeNum carrier) const
{
    // Check if carrier is in recent history (loop detection)
    for (int i = 0; i < 3; ++i) {
        if (p.recentCarriers[i] == carrier && carrier != 0) {
            return true; // Loop detected!
        }
    }
    return false;
}

bool DtnOverlayModule::cleanupPendingFromReceipt(const meshtastic_FwplusDtnData &receipt, const char *contextLabel)
{
    (void)receipt;
    (void)contextLabel;
    return false;
}

// Purpose: add carrier to loop detection circular buffer
void DtnOverlayModule::trackCarrier(Pending &p, NodeNum carrier)
{
    p.recentCarriers[p.recentCarrierIndex] = carrier;
    p.recentCarrierIndex = (p.recentCarrierIndex + 1) % 3;
    p.lastCarrier = carrier; // backward compat
}

NodeNum DtnOverlayModule::applyReceiptReturnHop(Pending &p, uint8_t targetIdx, bool allowStale)
{
    if (p.receiptReturnLen == 0) {
        p.receiptReturnIndex = 0;
        p.forceEdge = 0;
        p.receiptHopRetries = 0;
        p.receiptHopStartMs = millis();
        return 0;
    }

    if (targetIdx >= p.receiptReturnLen) {
        targetIdx = p.receiptReturnLen - 1;
    }

    p.receiptReturnIndex = targetIdx;
    p.receiptHopRetries = 0;
    p.receiptHopStartMs = millis();

    NodeNum forcedNext = 0;
    if (targetIdx < p.receiptReturnLen) {
        uint8_t hopByte = p.receiptReturnChain[targetIdx];
        forcedNext = resolveNextHopNode(hopByte);
        if (forcedNext == 0) {
            forcedNext = static_cast<NodeNum>(hopByte);
        }
        if (forcedNext == nodeDB->getNodeNum()) {
            forcedNext = 0;
        }
    }

    if (forcedNext != 0) {
        if (allowStale || isNodeReachable(forcedNext, configReceiptMaxNodeAgeSec)) {
            p.forceEdge = forcedNext;
            return forcedNext;
        }
    }

    p.forceEdge = 0;
    return 0;
}
bool DtnOverlayModule::degradeReceiptReturnHop(uint32_t id, Pending &p)
{
    if (!p.isReceiptSource || !p.hasReceiptReturnChain || p.receiptReturnLen == 0) {
        return false;
    }

    if (p.receiptReturnIndex == 0) {
        return false; // Already at earliest point in chain
    }

    int candidate = static_cast<int>(p.receiptReturnIndex) - 1;
    while (candidate >= 0 && candidate == static_cast<int>(p.receiptSelfIndex)) {
        candidate--;
    }

    uint8_t prevRetries = p.receiptHopRetries;

    while (candidate >= 0) {
        if (candidate == static_cast<int>(p.receiptSelfIndex)) {
            candidate--;
            continue;
        }

        uint8_t hopByte = p.receiptReturnChain[candidate];
        NodeNum hopNode = resolveNextHopNode(hopByte);
        if (hopNode == 0) {
            hopNode = static_cast<NodeNum>(hopByte);
        }
        if (hopNode == nodeDB->getNodeNum()) {
            candidate--;
            continue;
        }

        NodeNum applied = applyReceiptReturnHop(p, static_cast<uint8_t>(candidate), true);

        if (applied == 0 && candidate > 0) {
            candidate--;
            continue;
        }

        NodeNum logNode = applied != 0 ? applied : hopNode;
        tombstoneUntilMs.erase(id);
        LOG_WARN("DTN: Receipt id=0x%x fallback to return hop idx=%u node=0x%x (retries=%u)",
                 (unsigned)p.data.orig_id, (unsigned)p.receiptReturnIndex,
                 (unsigned)logNode, (unsigned)prevRetries);
        return true;
    }

    // No earlier hop available – drop forced edge and let adaptive logic work
    p.forceEdge = 0;
    p.hasReceiptReturnChain = false;
    p.receiptReturnIndex = 0;
    p.receiptHopRetries = 0;
    p.receiptHopStartMs = millis();
    LOG_WARN("DTN: Receipt id=0x%x exhausted return chain – falling back to adaptive routing",
             (unsigned)p.data.orig_id);
    return false;
}

// Purpose: get current epoch time in milliseconds (uint64 to avoid overflow)
uint64_t DtnOverlayModule::getEpochMs() const
{
    return (uint64_t)getValidTime(RTCQualityFromNet) * 1000ULL;
}
//FIX #153: Fixed TTL tail detection logic
// PROBLEM: Old logic checked if remaining <= (remaining * tailPercent / 100)
//          Example: remaining=300s, tail=20% → threshold=(300*20/100)=60s
//                   return (300 <= 60) = FALSE! (FAIL)
// ROOT CAUSE: Calculated threshold from REMAINING TTL, not INITIAL TTL
//             Should check: remaining <= (INITIAL * 20%)
// SOLUTION: Calculate threshold from initial TTL (configTtlMinutes)
// BENEFIT: Late fallback now works correctly for TTL tail scenarios
bool DtnOverlayModule::isInTtlTail(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const
{
    if (d.ttl_remaining_ms == 0) return true; // Already expired = in tail
    
    // Calculate INITIAL TTL from config (not remaining!)
    uint32_t initialTtl = configTtlMinutes * 60000; // Convert minutes to ms
    
    // Calculate tail threshold (e.g., 20% of INITIAL TTL)
    // Example: initialTtl=300s, tailPercent=20 → threshold=60s
    //          If remaining <= 60s → in tail (last minute of 5-minute TTL)
    uint32_t tailThreshold = (initialTtl * tailPercent) / 100;
    
    // In tail if remaining TTL <= threshold
    return (d.ttl_remaining_ms <= tailThreshold);
}

// Purpose: create or refresh a pending DTN entry and compute the next attempt time.
// Inputs: original message id and payload envelope; uses topology, mobility and per-destination spacing.
// Effect: updates election timing, applies far-node throttle, and stores per-dest last TX timestamp.
void DtnOverlayModule::scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d)
{
    bool isNewEntry = (pendingById.find(id) == pendingById.end());
    auto &p = pendingById[id];
    p.data = d;
    
    //Initialize time tracking for new entries
    if (isNewEntry) {
        uint32_t now = millis();
        p.lastAttemptMs = now;  // TTL decrement tracking
        p.captureTimeMs = now;  // Absolute timeout tracking
    }
    
    trackCarrier(p, nodeDB->getNodeNum()); // track ourselves as carrier
    
    //custody_path already initialized in enqueueFromCaptured (no duplicate add)
    
    // Calculate scheduling components using helper functions
    uint32_t base = calculateBaseDelay(d, p);
    uint32_t topologyDelay = calculateTopologyDelay(d);
    uint32_t mobilitySlot = calculateMobilitySlot(id, d, p);
    
    // For source node's first attempt: add delay if no routing confidence
    // This gives routing time to learn path via traceroute/beacons before we choose handoff target
    // Without this, tie-breaker picks wrong neighbor (lower NodeNum instead of correct direction)
    // Routing needs ~15-20s to build confidence=2 for stable route via DV-ETX
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    uint32_t routingWaitDelay = 0;
    if (isFromSource && p.tries == 0 && !hasSufficientRouteConfidence(d.orig_to)) {
        // Wait 15-20s for routing to learn path via traceroute/passive discovery
        // This is acceptable for DTN (opportunistic, non-realtime)
        routingWaitDelay = 15000 + (uint32_t)random(5001);
        LOG_INFO("DTN: Source without routing confidence - adding %u ms wait for route discovery (DV-ETX needs ~15-20s)", 
                 routingWaitDelay);
    }
    
    // Apply early bonus for confident routes
    uint32_t earlyBonus = 0;
    if (configPreferBestRouteSlotting && hasSufficientRouteConfidence(d.orig_to)) {
        earlyBonus = 1000; // pull in by ~1s
    }
    
    // Calculate target time
    uint32_t target = millis() + base + topologyDelay + mobilitySlot + routingWaitDelay;
    if (target > millis() + earlyBonus) target -= earlyBonus;
    
    // Apply per-destination spacing
    float mobility = fwplus_getMobilityFactor01();
    target = applyPerDestinationSpacing(target, d.orig_to, mobility);
    
    p.nextAttemptMs = target;
    LOG_DEBUG("DTN schedule id=0x%x next=%u ms (base=%u topology=%u slot=%u)", 
              id, (unsigned)(p.nextAttemptMs - millis()), (unsigned)base, 
              (unsigned)topologyDelay, (unsigned)mobilitySlot);
}
// Purpose: perform one forwarding decision for a pending DTN item.
// Steps: channel-util gate → route confidence/handoff → tail probe/fallback → overlay send and backoff scheduling.
// Guarantees: never emits plaintext via overlay; fallback uses native encrypted DM without spoofing sender.
void DtnOverlayModule::tryForward(uint32_t id, Pending &p)
{
    uint32_t now = millis(); //Get current time at start (used for TTL decrement and timeout checks)
    
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) {
        p.nextAttemptMs = now + 2500 + (uint32_t)random(500);
        LOG_DEBUG("DTN busy: defer id=0x%x", id);
        return;
    }

    // Check max tries limit (always respect deadline even if maxTries=0)
    // Only check deadline if we have valid time
    // FIX #66A: Fix off-by-one error in max_tries check
    // PROBLEM: p.tries >= configMaxTries gave up after 2 attempts (tries=0,1 then tries=2 >= 2)
    // SOLUTION: Use > instead of >= to allow configMaxTries attempts (tries=0,1,2 for maxTries=3)
    // NOTE: tries is incremented AFTER send (line 1653), so tries=2 means 2 attempts already made
    // FIX #84 + #87: Extended attempts for unknown routes or known routes with routing issues
    // PROBLEM: Unknown routes (hops=255) exhausted edge nodes after only 3 attempts
    // SOLUTION: Give 5 attempts for unknown source routes (vs 3 for known) to explore more edge candidates
    // BENEFIT: More progressive relay rotations = better mesh coverage, no broadcast overhead
    // FIX #87: Extended attempts for known routes with routing issues (NodeDB vs Router mismatch)
    // PROBLEM: NodeDB says hops=2 but Router has no next_hop → direct routing fails after 2 attempts
    // SOLUTION: Use progressive relay (5 attempts) for known routes when Router has no next_hop
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    uint32_t effectiveMaxTries = configMaxTries;
    
    if (hopsToDest == 255 && isFromSource && configMaxTries > 0) {
        // Unknown route - use extended tries (FIX #84)
        effectiveMaxTries = 5;  // Extended attempts for unknown routes from source
        // This allows progressive relay to try 5 different edge nodes before giving up
        if (p.tries == 0) {
            LOG_INFO("DTN: Unknown route to 0x%x - extending max_tries to %u (vs standard %u) for better edge coverage",
                     (unsigned)p.data.orig_to, (unsigned)effectiveMaxTries, (unsigned)configMaxTries);
        }
    } else if (hopsToDest != 255 && isFromSource && configMaxTries > 0) {
        // FIX #87: Check if Router has next_hop for known routes
        // If no next_hop, use progressive relay with extended tries
        if (!hasRouterNextHop(p.data.orig_to)) {
            effectiveMaxTries = 5;  // Extended tries for routing issues
            if (p.tries == 0) {
                LOG_INFO("DTN: Known route to 0x%x (hops=%u) but no Router next_hop - extending max_tries to %u for progressive relay",
                         (unsigned)p.data.orig_to, (unsigned)hopsToDest, (unsigned)effectiveMaxTries);
            }
        }
    }
    
    bool exceedsMaxTries = (effectiveMaxTries > 0 && p.tries > effectiveMaxTries);
    bool ttlExpired = (p.data.ttl_remaining_ms == 0);
    
    // Absolute timeout for invalid/unreachable destinations
    // PROBLEM: Invalid dest (e.g., Node29 when max=Node17) causes infinite pending
    // SOLUTION: Hard timeout after 5 minutes from capture, regardless of TTL/max_tries
    const uint32_t MAX_PENDING_TIME_MS = 5 * 60 * 1000; // 5 minutes
    uint32_t age = now - p.captureTimeMs;  // Age from CAPTURE, not last attempt!
    bool absoluteTimeout = (age > MAX_PENDING_TIME_MS);
    
    if (exceedsMaxTries || ttlExpired || absoluteTimeout) {
        const char* reason = exceedsMaxTries ? "max tries" : 
                            ttlExpired ? "ttl expired" : "absolute timeout";
        
        // FIX: Reference invalidation by emitReceipt()
        // PROBLEM: emitReceipt() → enqueueFromCaptured() → std::map::insert() → rehash
        //          This INVALIDATES the reference 'p' passed to tryForward()!
        //          Any access to 'p' after emitReceipt() → SEGFAULT
        // ROOT CAUSE: tryForward() signature: void tryForward(uint32_t id, Pending &p)
        //             'p' is a reference into pendingById, insert() invalidates all references
        // SOLUTION: Save needed values from 'p' BEFORE calling emitReceipt()
        // BENEFIT: Prevents crash when giving up packets (max tries/timeout)
        uint32_t origFrom = p.data.orig_from;
        uint32_t origTo = p.data.orig_to;
        uint32_t tries = p.tries;
        uint32_t ttlRemaining = p.data.ttl_remaining_ms;
        
        //preserve receipt hash for tombstone after give up
        bool wasReceipt = p.isReceiptCustody;
        uint32_t receiptCustodyHash = 0;
        bool hasReceiptHash = false;
        uint8_t receiptStatusByte = 0;
        if (wasReceipt && p.data.payload.size >= 3) {
            receiptStatusByte = p.data.payload.bytes[2];
            uint32_t hash = 0x811C9DC5;
            hash = (hash ^ p.data.orig_id) * 0x01000193;
            hash = (hash ^ (uint32_t)p.data.orig_to) * 0x01000193;
            hash = (hash ^ (uint32_t)receiptStatusByte) * 0x01000193;
            hash = (hash ^ (uint32_t)p.data.orig_from) * 0x01000193;
            receiptCustodyHash = hash;
            hasReceiptHash = true;
        }

        emitReceipt(origFrom, id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0);

        //prevent immediate re-capture once we abandon this id
        uint32_t tombstoneDuration = configTombstoneMs;
        if (ttlRemaining && ttlRemaining > tombstoneDuration) {
            tombstoneDuration = ttlRemaining;
        }
        if (tombstoneDuration) {
            tombstoneUntilMs[id] = now + tombstoneDuration;
            if (hasReceiptHash) {
                tombstoneUntilMs[receiptCustodyHash] = now + tombstoneDuration;
            }
        }

        if (hasReceiptHash) {
            deliveredReceiptIds.erase(receiptCustodyHash);
        }
        
        // NOTE: 'p' is INVALID after emitReceipt() - use saved values!
        LOG_WARN("DTN give up id=0x%x tries=%u/%u reason=%s ttl_ms=%u age_ms=%u", id, (unsigned)tries, 
                 (unsigned)effectiveMaxTries, reason, (unsigned)ttlRemaining, age);
        
        // FIX #50: Log custody timeout details (use saved values)
        LOG_INFO("DTN: custody timeout id=0x%x to=0x%x after %u attempts (max=%u)", 
                 (unsigned)id, (unsigned)origTo, (unsigned)tries, (unsigned)effectiveMaxTries);
        
        pendingById.erase(id);
        ctrGiveUps++;
        return;
    }

    // Check DV-ETX route confidence gating
    LOG_DEBUG("DTN: About to check hasSufficientRouteConfidence for dest=0x%x", (unsigned)p.data.orig_to);
    bool lowConf = !hasSufficientRouteConfidence(p.data.orig_to);
    LOG_DEBUG("DTN: hasSufficientRouteConfidence returned, lowConf=%d", lowConf);
    // isFromSource already declared above (line 1453)
    
    // For source without routing confidence: trigger traceroute
    // Traceroute builds routing confidence (1 ACK = confidence 1 = NextHop ready)
    // This helps DTN choose correct handoff direction in subsequent attempts
    // FIX #81 + #82: For unknown routes (hops=255), skip traceroute wait and use progressive relay immediately
    // PROBLEM: Traceroute defer created infinite loop (p.tries never incremented) + wasted 45s when traceroute failed
    // SOLUTION: Try progressive relay immediately for unknown routes instead of waiting for traceroute
    if (lowConf && isFromSource && p.tries == 0) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        if (hopsToDest == 255) {  // Unknown route
            // FIX #82: Skip traceroute defer, try progressive relay immediately
            // Rationale: For hop_limit=3 scenarios, traceroute often fails (path too long)
            //            Progressive relay has proven effective (Test 3b: 39s delivery via 3-hop custody chain)
            LOG_INFO("DTN: Unknown route to 0x%x - skipping traceroute, will try progressive relay",
                     (unsigned)p.data.orig_to);
            // Don't defer, don't return - continue to progressive relay logic below
        } else {
            //COMMENTED OUT: maybeTriggerTraceroute function may not exist or may crash
            // Known route but low confidence - trigger traceroute to improve routing
            // maybeTriggerTraceroute(p.data.orig_to);
            LOG_INFO("DTN: Source without routing confidence to 0x%x (try=%u, hops=%u) - traceroute skipped",
                     (unsigned)p.data.orig_to, (unsigned)p.tries, (unsigned)hopsToDest);
        }
    }
    
    // FIX #54 + #82: For intermediates with unknown routes, skip defer and try custody immediately
    // FIX #52 was too aggressive - blocked all low-confidence intermediates
    // FIX #82 extension: Skip traceroute defer for intermediates too (same rationale as source)
    if (p.tries == 0 && !isFromSource && lowConf) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        if (hopsToDest == 255) {  // ONLY unknown routes
            // FIX #82: Skip defer, let intermediate try custody/progressive relay immediately
            LOG_INFO("DTN: Intermediate unknown route to 0x%x - will try custody transfer",
                     (unsigned)p.data.orig_to);
            // Don't defer, don't return - continue to custody logic below
        }
        // Known route (hops < 255) but low confidence: try custody anyway!
        // Traceroute may not help (no response), but custody chain can still work
        LOG_DEBUG("DTN: Intermediate has known route to 0x%x (hops=%u) despite low confidence - trying custody",
                 (unsigned)p.data.orig_to, (unsigned)hopsToDest);
    }
        
    if (lowConf && isFromSource) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        
        // FIX #79: Use HOP_RELIABLE (3) instead of HOP_MAX (7) for progressive relay threshold
        // PROBLEM: Simulator uses hop_limit=3, Router has no next_hop for dest beyond this!
        // SOLUTION: Progressive relay when dest > HOP_RELIABLE (3) instead of HOP_MAX (7)
        //FIX #178: Cache chooseProgressiveRelay result to prevent double-call bug
        // PROBLEM: chooseProgressiveRelay called TWICE in same attempt:
        //          1) Line 2226: Check if available → selects 0x1b, adds to progressiveRelays[]
        //          2) Line 4796 (selectForwardTarget): Actual use → sees 0x1b as "already tried", selects 0x13!
        //          Result: Wrong edge node selected (second choice instead of best choice)
        // ROOT CAUSE: First call modifies progressiveRelays[] before second call
        // SOLUTION: Call once, cache result in PendingDtn, reuse in selectForwardTarget
        // BENEFIT: Correct edge node selection (first choice = best choice!)
        
        // FIX #89: Progressive relay for known routes with routing issues (no Router next_hop)
        // PROBLEM: NodeDB has hops_away but Router has no next_hop → direct routing fails
        // SOLUTION: Use progressive relay for known routes when Router has no next_hop
        // Try progressive relay for: unknown (hops=255), very distant (hops>HOP_RELIABLE), or no Router next_hop
        bool noRouterNextHop = (hopsToDest != 255 && !hasRouterNextHop(p.data.orig_to));
        if (hopsToDest == 255 || hopsToDest > HOP_RELIABLE || noRouterNextHop) {
            // FIX #178: Cache progressive relay choice (prevent double-call)
            NodeNum edgeNode = chooseProgressiveRelay(p.data.orig_to, p);
            p.cachedProgressiveRelay = edgeNode; // Cache for selectForwardTarget
            
            if (edgeNode != 0) {
                // SUCCESS: Progressive relay available
                // Continue to DTN forwarding below (selectForwardTarget will use cached edge node)
                if (noRouterNextHop) {
                    LOG_INFO("DTN: Progressive relay available to edge 0x%x (dest 0x%x hops=%u, no Router next_hop) - using DTN custody",
                             (unsigned)edgeNode, (unsigned)p.data.orig_to, hopsToDest);
                } else {
                    LOG_INFO("DTN: Progressive relay available to edge 0x%x (dest 0x%x hops=%u) - using DTN custody",
                             (unsigned)edgeNode, (unsigned)p.data.orig_to, hopsToDest);
                }
            } else {
                p.cachedProgressiveRelay = 0; // No edge node available
                //NEW: Try more custody attempts BEFORE broadcast for far destinations
                // PROBLEM: Far destinations (5+ hops) immediately fall back to broadcast after try 0 fails
                //          Example: Node13→Node1, try 0 via 0x1b → fail, try 1 → broadcast (not custody!)
                //          Result: No alternative custody paths explored, immediate broadcast storm
                // ROOT CAUSE: chooseProgressiveRelay returns 0 when first edge blocked → immediate broadcast
                // SOLUTION: Delay broadcast until try >= 2 for far destinations
                //           Try 0-1: Attempt custody via progressive relay (may use different edge nodes)
                //           Try 2+: Broadcast fallback as last resort
                // RATIONALE: Progressive relay can select different edges across retries (rotation)
                //            FIX #134 resets progressiveRelays[] after broadcast → enables node reuse
                //            More custody attempts = better chance of delivery before broadcast
                //NEW: Clear progressive relay chain to allow alternative custody attempts
                // PROBLEM: After first progressive relay fail, chooseProgressiveRelay returns 0 forever
                //          Because tried nodes remain in progressiveRelays[] array → "already in chain"
                // SOLUTION: Clear progressiveRelays[] when no edge found on early attempts (try < 2)
                //           This allows algorithm to retry same nodes (they may become available)
                // BENEFIT: Try 1 can reuse nodes from try 0 if paths recovered (link health improved)
                
                bool isUnknownDest = (hopsToDest == 255);
                bool isFarDest = (hopsToDest > 3);  // Beyond native reach (hopLimit=3)
                
                // For far/unknown destinations, try custody multiple times before broadcast
                uint32_t custodyAttemptsBeforeBroadcast = (isUnknownDest || isFarDest) ? 2 : 1;
                
                if (p.tries < custodyAttemptsBeforeBroadcast) {
                    // Clear progressive relay chain to enable alternative custody attempts
                    if (p.progressiveRelayCount > 0) {
                        LOG_INFO("DTN: Clearing progressive relay chain (try=%u) to allow alternative custody attempts for dest 0x%x",
                                (unsigned)p.tries, (unsigned)p.data.orig_to);
                        p.progressiveRelayCount = 0;
                        memset(p.progressiveRelays, 0, sizeof(p.progressiveRelays));
                    }
                    
                    // Continue to normal custody forwarding - will retry with fresh progressive relay selection
                    LOG_INFO("DTN: No progressive relay on try=%u for dest 0x%x (hops=%u) - will retry custody (max %u attempts before broadcast)",
                            (unsigned)p.tries, (unsigned)p.data.orig_to, hopsToDest, custodyAttemptsBeforeBroadcast);
                    // Fall through to DTN forwarding below (will use direct forward or selectForwardTarget)
                } else {
                    // After custody attempts exhausted, try broadcast rescue
                    bool shouldTryBroadcast = (isUnknownDest || isFarDest) && p.tries >= custodyAttemptsBeforeBroadcast;
                    bool allowBroadcastRescue = shouldTryBroadcast;
                    
                    if (allowBroadcastRescue) {
                        // Try broadcast rescue for unknown/far destinations (last resort after custody attempts)
                        LOG_WARN("DTN: No progressive relay available for dest 0x%x (hops=%u) after %u custody attempts - trying broadcast rescue (try=%u)",
                                (unsigned)p.data.orig_to, hopsToDest, custodyAttemptsBeforeBroadcast, (unsigned)p.tries);
                        
                        // Call tryIntelligentFallback which contains broadcast rescue logic
                        if (tryIntelligentFallback(id, p)) {
                            return; // Broadcast sent or other intelligent fallback succeeded
                        }
                        
                        // Broadcast failed (cooldown, channel_util, or other gates) - try Stock fallback
                        LOG_WARN("DTN: Broadcast rescue blocked/failed for dest 0x%x - trying Stock fallback as last resort",
                                (unsigned)p.data.orig_to);
                    }
                }
                
                // NO EDGE NODES: Try Stock fallback as last resort
            if (p.data.allow_proxy_fallback) {
                //FIX #145: Copy dest before sendProxyFallback (p is invalid after erase!)
                // PROBLEM: sendProxyFallback() calls erase(id) → p becomes dangling reference
                //          If we access p.data after sendProxyFallback() returns false → CRASH!
                // SOLUTION: Copy p.data.orig_to before calling sendProxyFallback
                NodeNum destCopy = p.data.orig_to;
                LOG_WARN("DTN: No progressive relay available for dest 0x%x (hops=%u) - trying native fallback",
                         (unsigned)destCopy, hopsToDest);
                sendProxyFallback(id, p);
                ctrFallbacksAttempted++;
                return; // Always return - pending is erased, p is invalid
                }
            }
        } else {
            // FIX #53: Don't fallback if DTN can help - try DTN custody instead!
            // Known route (hops < 255 and <= HOP_RELIABLE) but low confidence
            // Check if DTN can help with this destination (hops >= 2)
            bool dtnCanHelp = canDtnHelpWithDestination(p.data.orig_to);
            
            if (dtnCanHelp) {
                // DTN should handle this (hops >= 2), don't fallback prematurely!
                LOG_INFO("DTN: Known route but low confidence to dest 0x%x (hops=%u) - trying DTN custody (not fallback)",
                         (unsigned)p.data.orig_to, hopsToDest);
                // Continue to DTN custody logic below (don't return)
            } else {
                // DTN can't help (hops < 2), use native fallback
                if (p.data.allow_proxy_fallback) {
                    //FIX #145: Copy dest before sendProxyFallback (p is invalid after erase!) - part 2
                    NodeNum destCopy = p.data.orig_to;
                    LOG_WARN("DTN: Known route but low confidence to dest 0x%x (hops=%u) - using native fallback (DTN won't help)",
                             (unsigned)destCopy, hopsToDest);
                    sendProxyFallback(id, p);
                    ctrFallbacksAttempted++;
                    return; // Always return - pending is erased, p is invalid
                }
            }
        }
    }
    
    //fw+
    // FIX: Intermediate nodes also need progressive relay for unknown destinations
    // PROBLEM: Node6 receives custody for dest=0x1d (unknown, hops=255)
    //          Only source nodes (isFromSource) call chooseProgressiveRelay() above
    //          Intermediate nodes skip directly to fallback → no progressive relay!
    //          Result: Node6 tries direct send + broadcast, never tries progressive relay
    // ROOT CAUSE: Progressive relay logic (line 2214-2347) is ONLY for isFromSource
    //             Intermediate nodes need progressive relay too for unknown/far destinations
    // SOLUTION: Add progressive relay check for intermediate nodes before fallback
    // BENEFIT: Full DTN custody chain works - each hop can choose next best relay
    if (!isFromSource) {
        LOG_DEBUG("DTN: Intermediate node check - about to evaluate progressive relay");
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        bool needsProgressiveRelay = (hopsToDest == 255 || hopsToDest > HOP_RELIABLE);
        LOG_DEBUG("DTN: needsProgressiveRelay=%d (hopsToDest=%u, HOP_RELIABLE=%d)", needsProgressiveRelay, hopsToDest, HOP_RELIABLE);
        
        if (needsProgressiveRelay && p.cachedProgressiveRelay == 0) {
            // Try progressive relay for intermediate with unknown/far dest
            LOG_DEBUG("DTN: About to call chooseProgressiveRelay for intermediate (dest=0x%x)", (unsigned)p.data.orig_to);
            NodeNum edgeNode = chooseProgressiveRelay(p.data.orig_to, p);
            LOG_DEBUG("DTN: chooseProgressiveRelay returned edgeNode=0x%x", (unsigned)edgeNode);
            p.cachedProgressiveRelay = edgeNode;
            
            if (edgeNode != 0) {
                LOG_INFO("DTN: Intermediate unknown route to 0x%x - will try custody transfer via 0x%x",
                         (unsigned)p.data.orig_to, (unsigned)edgeNode);
                // Continue to DTN forwarding (selectForwardTarget will use cached edge)
            } else {
                LOG_INFO("DTN: Intermediate unknown route to 0x%x - will try custody transfer",
                         (unsigned)p.data.orig_to);
                // No progressive relay available, try direct/fallback below
            }
        }
    }
    
    // FIX #55: Removed duplicate intermediate defer (1467-1475)
    // FIX #54 already handles selective defer for intermediates (unknown routes only)
    // Old code was blocking ALL lowConf intermediates, breaking custody chains!
    
    //Fast path for direct neighbors - immediate DTN forwarding
    if (isDirectNeighbor(p.data.orig_to)) {
        // For direct neighbors, skip fallback checks and go straight to DTN forwarding
        LOG_DEBUG("DTN: Fast path for direct neighbor 0x%x", (unsigned)p.data.orig_to);
        // Continue to DTN forwarding below
    } else {
        // For source and intermediate nodes: try various fallback mechanisms
        // Note: traceroute is already triggered above (line 1226) for source without confidence
        if (isFromSource) {
            // fw+ Allow broadcast fallback for source in TTL tail as last resort
            if (tryIntelligentFallback(id, p)) return;
        } else {
            // Intermediate nodes: try intelligent fallback first
            if (tryIntelligentFallback(id, p)) return;
            // Try various fallback mechanisms for multi-hop destinations
            if (tryNearDestinationFallback(id, p)) return;
            if (tryKnownStockFallback(id, p)) return;
        }
        
        //Try fallback for unresponsive FW+ destinations (both source and intermediate)
        if (tryFwplusUnresponsiveFallback(id, p)) return;
    }

    //==============================================================================
    // ADAPTIVE PATH SELECTION: Check if we should reroute due to path failure
    //==============================================================================
    
    if (p.isReceiptCustody && p.isReceiptSource && p.hasReceiptReturnChain) {
        const uint8_t kReceiptHopRetryThreshold = 2;
        if (p.receiptHopRetries >= kReceiptHopRetryThreshold) {
            degradeReceiptReturnHop(id, p);
        }
    }

    NodeNum target = 0;
    bool useAdaptiveReroute = configEnableAdaptiveRerouting;
    
    if (useAdaptiveReroute) {
        // Get default target first
        NodeNum currentTarget = selectForwardTarget(p);
        bool shouldReroute = false;
        
        // Reroute Trigger 1: Current target link is unhealthy (proactive avoidance)
        if (currentTarget != 0 && currentTarget != p.data.orig_to && !isLinkHealthy(currentTarget)) {
            LOG_WARN("DTN: Next hop 0x%x is unhealthy - attempting adaptive reroute", 
                     currentTarget);
            shouldReroute = true;
        }
        
        // Reroute Trigger 2: Path to destination is known to be unreliable (learned from history)
        if (currentTarget != 0 && currentTarget != p.data.orig_to && 
            !isPathReliable(currentTarget, p.data.orig_to)) {
            LOG_WARN("DTN: Path via 0x%x to 0x%x is unreliable - attempting adaptive reroute",
                     currentTarget, p.data.orig_to);
            shouldReroute = true;
        }
        
        // Reroute Trigger 3: Multiple failures on current approach (reactive rerouting)
        if (p.tries >= 2) {
            auto& history = packetPathHistoryMap[id];
            if (history.pathSwitchCount == 0) { // Haven't switched yet
                LOG_WARN("DTN: Packet id=0x%x failed %u times - attempting adaptive reroute",
                         id, p.tries);
                shouldReroute = true;
            } else if (p.tries >= (history.pathSwitchCount + 1) * 2) {
                // After switching, still failing - try another path
                LOG_WARN("DTN: Alternative path also failing (tries=%u switches=%u) - trying another route",
                         p.tries, history.pathSwitchCount);
                shouldReroute = true;
            }
        }
        
        if (shouldReroute) {
            // Attempt to find alternative path
            NodeNum altTarget = selectAlternativePathOnFailure(id, p, currentTarget);
            
            if (altTarget != 0) {
                // SUCCESS: Use alternative path
                target = altTarget;
                LOG_INFO("DTN: Using adaptive alternative 0x%x (original was 0x%x)",
                         target, currentTarget);
            } else {
                // FAILURE: No alternative found, use original
                target = currentTarget;
                LOG_WARN("DTN: No viable alternative for id=0x%x, using original 0x%x",
                         id, currentTarget);
            }
        } else {
            // No rerouting needed, use default target
            target = currentTarget;
        }
        
        // Record this path attempt for learning (if not direct delivery)
        if (target != 0 && target != p.data.orig_to) {
            recordPathAttempt(target, p.data.orig_to, id);
        }
        
    } else {
        // Adaptive rerouting disabled, use default selection
        target = selectForwardTarget(p);
    }
    
    //FIX #109: Handle Stock detection from intermediate intelligence
    // PROBLEM: Intermediate detected dest is Stock (NodeInfo but NO beacon)
    // SOLUTION: selectForwardTarget() returns 0, send FAILED receipt and abort
    if (target == 0 && !isFromSource) {
        // Intermediate detected Stock destination - send FAILED receipt
        LOG_WARN("DTN: Intermediate aborting custody for Stock dest 0x%x (no DTN route available)",
                 (unsigned)p.data.orig_to);
        
        emitReceipt(p.data.orig_from, p.data.orig_id,
                   meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                   meshtastic_Routing_Error_NO_ROUTE);

        pendingById.erase(id);
        return; // Don't retry - DTN can't help Stock destinations
    }
    
    //Receipt reverse-path guard: avoid hammering stale links
    if (p.isReceiptCustody && configReceiptMaxNodeAgeSec) {
        bool destFresh = isNodeReachable(p.data.orig_to, configReceiptMaxNodeAgeSec);
        bool targetFresh = (target != 0) ? isNodeReachable(target, configReceiptMaxNodeAgeSec) : false;
        if (target == p.data.orig_to) {
            targetFresh = destFresh;
        }

        if (!destFresh || !targetFresh) {
            //Allow one forced attempt when reverse path looks stale (sim PKI receipts)
            if (p.tries == 0) {
                LOG_WARN("DTN: Receipt id=0x%x stale reverse path (destFresh=%u targetFresh=%u) - forcing initial attempt",
                         (unsigned)p.data.orig_id, (unsigned)destFresh, (unsigned)targetFresh);
                p.forceEdge = NO_NEXT_HOP_PREFERENCE;
            } else {
                LOG_WARN("DTN: Receipt id=0x%x aborted - stale reverse path (destFresh=%u targetFresh=%u)",
                         (unsigned)p.data.orig_id, (unsigned)destFresh, (unsigned)targetFresh);

                emitReceipt(p.data.orig_from, p.data.orig_id,
                            meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                            meshtastic_Routing_Error_NO_ROUTE);

                if (configTombstoneMs) {
                    tombstoneUntilMs[id] = now + configTombstoneMs;
                }
                deliveredReceiptIds.erase(id);
                pendingById.erase(id);
                return;
            }
        }
    }
    
    // Decrement TTL before forwarding (each hop consumes time)
    // Calculate elapsed time since last attempt or capture (includes first forward delay!)
    uint32_t elapsed = now - p.lastAttemptMs;  // Always decrement (includes capture→forward delay)
    
    // Decrement TTL by elapsed time (minimum 1ms to ensure progress)
    if (elapsed > 0) {
        if (p.data.ttl_remaining_ms > elapsed) {
            p.data.ttl_remaining_ms -= elapsed;
        } else {
            p.data.ttl_remaining_ms = 0; // Expired during wait
        }
        LOG_DEBUG("DTN: TTL decrement id=0x%x elapsed=%u ttl_remaining=%u", 
                 id, elapsed, (unsigned)p.data.ttl_remaining_ms);
    }
    
    // Double-check TTL after decrement
    if (p.data.ttl_remaining_ms == 0) {
        LOG_WARN("DTN: Packet id=0x%x expired after TTL decrement (elapsed=%u)", id, elapsed);
        emitReceipt(p.data.orig_from, id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0);
        pendingById.erase(id);
        ctrExpired++;
        return;
    }
    
    // Send DTN overlay packet
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    if (p.isReceiptVariant) {
        meshtastic_FwplusDtnReceipt receiptToSend = p.receiptMsg;
        receiptToSend.dest = p.data.orig_to;
        receiptToSend.custody_id = p.data.orig_id;
        receiptToSend.ttl_remaining_ms = p.data.ttl_remaining_ms;
        receiptToSend.allow_proxy_fallback = p.data.allow_proxy_fallback;
        receiptToSend.custody_path_count = 0;
        if (p.data.custody_path_count > 0) {
            pb_size_t copyCount = std::min<pb_size_t>(p.data.custody_path_count, (pb_size_t)sizeof(receiptToSend.custody_path));
            receiptToSend.custody_path_count = copyCount;
            for (pb_size_t i = 0; i < copyCount; ++i) {
                receiptToSend.custody_path[i] = p.data.custody_path[i];
            }
        }
        // Keep pending copy in sync
        p.receiptMsg = receiptToSend;
        msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
        msg.variant.receipt = receiptToSend;
    } else {
        msg.which_variant = meshtastic_FwplusDtn_data_tag;
        msg.variant.data = p.data;
    }

    meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
    if (!mp) {
        p.nextAttemptMs = millis() + 3000;
        return;
    }
    // Configure packet
    mp->to = target;
    mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    mp->want_ack = false;
    mp->decoded.want_response = false;
    //Debug: log routing information for custody packet
    meshtastic_NodeInfoLite *targetInfo = nodeDB->getMeshNode(target);
    uint8_t targetNextHop = targetInfo ? targetInfo->next_hop : NO_NEXT_HOP_PREFERENCE;
    uint8_t targetHops = targetInfo ? targetInfo->hops_away : 255;
    LOG_INFO("DTN: Sending custody to target=0x%x hop_limit=%u hops_away=%u next_hop=%u", 
             (unsigned)target, mp->hop_limit, (unsigned)targetHops, (unsigned)targetNextHop);
    LOG_DEBUG("DTN: Custody packet will be %s by Router", 
             targetNextHop == NO_NEXT_HOP_PREFERENCE ? "FLOODED (no next_hop)" : "UNICAST (has next_hop)");
    setPriorityForTailAndSource(mp, p, isFromSource);
    // Check if we should back off (favor closer relayers)
    // This check is for OVERHEARD packets (foreign capture), not custody transfers
    // If we received unicast handoff (mp.to == us), we MUST forward - that's the point of custody transfer
    // Only defer if we're NOT closer to destination than source (passive observation optimization)
    if (!isFromSource) {
        // DISABLED: This check blocks valid custody transfers from intermediate nodes
        // Example: Node1→Node2 handoff, Node2 is 0 hops from Node1, 2 hops from Node6
        //          Check: 2 > 0 → defer ← WRONG! Node2 should forward!
        // TODO: Re-enable only for foreign (overheard) capture, not unicast custody transfers
        /*
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        uint8_t hopsToSrc = getHopsAway(p.data.orig_from);
        if (hopsToSrc != 255 && hopsToDest != 255 && hopsToDest > hopsToSrc) {
            // FIX #77: Use adaptive backoff based on destination distance
            uint32_t adaptiveBackoff = getAdaptiveRetryBackoff(p.data.orig_to);
            p.nextAttemptMs = millis() + adaptiveBackoff + 5000 + (uint32_t)random(2000);
            LOG_DEBUG("DTN: Deferring - dest further than src (hops: dst=%u src=%u, backoff=%u ms)", 
                     (unsigned)hopsToDest, (unsigned)hopsToSrc, adaptiveBackoff);
            return;
        }
        */
    }

    // FIX #50: Add custody ACK tracking for debugging
    // FIX #80: Log custody chain for debugging and route visualization
    std::ostringstream pathOss;
    for (pb_size_t i = 0; i < p.data.custody_path_count; ++i) {
        if (i > 0) pathOss << "->";
        pathOss << "0x" << std::hex << (unsigned)p.data.custody_path[i];
    }
    LOG_INFO("DTN: custody send id=0x%x to=0x%x edge=0x%x try=%u ttl_ms=%u chain=[%s]", 
             (unsigned)id, (unsigned)target, (unsigned)mp->to, (unsigned)p.tries, 
             (unsigned)p.data.ttl_remaining_ms, pathOss.str().c_str());
    
    //FIX #150: Treat receipts like DATA packets (same progressive relay mechanism)
    // PROBLEM: FIX #149c added complex broadcast fallback for receipts (dead-end detection, TTL tail)
    //          Result: Receipts blocked by broadcast cooldown/gates → never reach source (FAIL)
    //          Meanwhile DATA packets work reliably via progressive relay + Router flooding
    // ROOT CAUSE: Special handling for receipts bypassed proven DATA packet delivery path
    // SOLUTION: Remove special receipt handling - let Router handle flooding like for DATA
    //           - Receipts use same progressive relay as DATA (custody chain)
    //           - Router decides broadcast vs unicast (DV-ETX routing)
    //           - No special gates/cooldown checks for receipts
    // BENEFIT: Receipts inherit DATA packet reliability, simplified code (-60 lines)
    // NOTE: If no Router next_hop, packet proceeds to sendToMesh() which floods (like DATA)
    
    // Send the packet
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    p.lastAttemptMs = now; //Track attempt time for next TTL decrement
    ctrForwardsAttempted++;

    if (p.isReceiptCustody && p.isReceiptSource && p.hasReceiptReturnChain && p.forceEdge != 0 && target == p.forceEdge) {
        p.receiptHopRetries++;
    }
    if (p.isReceiptCustody) {
        uint32_t tombstoneDuration = configTombstoneMs;
        if (p.data.ttl_remaining_ms && p.data.ttl_remaining_ms > tombstoneDuration) {
            tombstoneDuration = p.data.ttl_remaining_ms;
        }
        if (tombstoneDuration) {
            tombstoneUntilMs[id] = now + tombstoneDuration;
        }

        deliveredReceiptIds.erase(id);

        if (!p.isReceiptSource) {
            pendingById.erase(id);
            return;
        }
    }
    lastForwardMs = now;
    
    //FIX #108b: Send DTN TRANSMITTED receipt to APK after first custody send
    // PROBLEM: APK doesn't know when DTN actually transmitted packet (15-20s route discovery delay)
    //          User confused: "Did it send? Is it stuck?"
    // SOLUTION: Send PROGRESSED receipt after actual RF transmission (first try only)
    //           UX: "Packet transmitted" (RF icon, no more waiting)
    // NOTE: Only send for source node (not intermediate) and only on first try
    bool isSourceNode = (p.data.orig_from == nodeDB->getNodeNum());
    if (isSourceNode && p.tries == 1) {
        // Send TRANSMITTED receipt (reason=0 indicates "transmitted by source")
        emitReceipt(p.data.orig_from, p.data.orig_id, 
                   meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED,
                   0); // reason=0 = transmitted by source
        LOG_INFO("DTN: Sent TRANSMITTED receipt to APK for id=0x%x (UX: 'Packet on-air')", (unsigned)p.data.orig_id);
    } 
    
    //FIX #151 REMOVED - replaced by FIX #166 (single attempt, no broadcast fallback)
    // OLD: Tried broadcast fallback after unicast failure (try=1)
    // NEW: Erase receipt immediately after custody send (try=1) - simpler, prevents accumulation
    // BENEFIT: No retry loops, cleaner pending queue, receipts don't accumulate
    
    // Update tracking
    lastDestTxMs[p.data.orig_to] = millis();
    if (target != p.data.orig_to) {
        auto &ph = preferredHandoffByDest[p.data.orig_to];
        ph.node = target;
        ph.lastUsedMs = millis();
        ctrHandoffsAttempted++; //metric: custody handoff to another FW+ node
        
        //FIX #103: Custody transfer = transfer of responsibility
        // PROBLEM: Intermediate node keeps packet in pending after handoff → sends EXPIRED 5min later (zbędne!)
        //          Example: Node1→Node2 handoff, Node2→Node3 handoff, packet delivered
        //                   Node1 gets DELIVERED (OK), Node2 sends EXPIRED 5min later (FAIL)
        // ROOT CAUSE: Intermediate node trzyma pending "na wszelki wypadek" mimo handoff
        // SOLUTION: Intermediate node (NOT source) USUWANY pending po handoff
        //           Odpowiedzialność przeszła na następny node!
        // NOTE: Source node (isFromSource==true) TRZYMA pending - czeka na DELIVERED receipt
        if (!isFromSource) {
            LOG_INFO("DTN: Intermediate handoff complete for id=0x%x to edge=0x%x - erasing pending (custody transferred)",
                     (unsigned)id, (unsigned)target);
            pendingById.erase(id);
            return; // Pending erased, no need to schedule retry
        }
    }
    
    //Track DTN attempt for unresponsive detection (only for direct-to-dest attempts, not handoffs)
    if (target == p.data.orig_to && isFwplus(p.data.orig_to)) {
        p.lastDtnAttemptMs = millis();
        p.dtnFailedAttempts++; // Will be reset if we get a receipt
        LOG_DEBUG("DTN: Tracking attempt %u to FW+ dest 0x%x (unresponsive detection)", 
                 (unsigned)p.dtnFailedAttempts, (unsigned)p.data.orig_to);
    }
    
    // Schedule next attempt (only for source node or direct-to-dest)
    p.nextAttemptMs = millis() + configRetryBackoffMs;
    LOG_INFO("DTN fwd overlay id=0x%x dst=0x%x try=%u next=%u ms", id, (unsigned)p.data.orig_to, (unsigned)p.tries,
             (unsigned)(p.nextAttemptMs - millis()));
}
// Purpose: perform late native DM fallback for a DTN item (encrypted), preserving original id for ACK mapping.
// Returns: true if a send was attempted or queued; schedules next retry on success or alloc failure.
bool DtnOverlayModule::sendProxyFallback(uint32_t id, Pending &p)
{
    //FIX #126: Guard against dest=0 (part 2 - prevents crash in allocCopy/sendToMesh)
    // PROBLEM: If malformed packet passed FIX #126 guard in enqueueFromCaptured (shouldn't happen),
    //          sendProxyFallback would crash on dm->to = 0 (line 2451)
    // SOLUTION: Double-check here and abort gracefully
    if (p.data.orig_to == 0 || p.data.orig_from == 0) {
        LOG_ERROR("DTN FIX #126: Aborting fallback for malformed packet id=0x%x from=0x%x to=0x%x (invalid NodeNum)",
                 id, p.data.orig_from, p.data.orig_to);
        pendingById.erase(id);
        return false; // Abort, prevent crash
    }
    
    bool isSourceFallback = (p.data.orig_from == nodeDB->getNodeNum());
    
    //FIX #106a: DISABLE intermediate fallback to Stock
    // PROBLEM: Multiple intermediates → multiple fallback DMs → duplicates + coordinator problem
    // ROOT CAUSE: Each intermediate tries to fallback → race condition → Stock node gets N copies
    // SOLUTION: Only SOURCE can fallback to Stock, intermediate sends FAILED receipt
    // NOTE: Source has context (user waiting), intermediate doesn't (custody holder)
    if (!isSourceFallback) {
        LOG_WARN("DTN: Intermediate fallback disabled for dest=0x%x (coordinator problem)", 
                 (unsigned)p.data.orig_to);
        emitReceipt(p.data.orig_from, p.data.orig_id, 
                   meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
                   meshtastic_Routing_Error_NO_ROUTE);
        pendingById.erase(id);
        return false; // Don't retry
    }
    
    //FIX #106b: Source fallback ABORTS DTN (no re-capture loop)
    // PROBLEM: Fallback DM → sendToMesh → Router intercept → DTN re-capture → infinite loop!
    // ROOT CAUSE: Tombstone created AFTER sendToMesh, so re-capture happens first
    // SOLUTION: Erase pending BEFORE sendToMesh → packet won't be in DTN tracking → no re-capture
    //           Send fresh native DM (new ID, no tombstone conflict)
    
    //FIX #120b: DON'T mark as Stock if dest is known FW+ (prevents intercept blocking)
    // PROBLEM: Progressive relay exhausted → Stock fallback → dest marked as Stock
    //          Next message to same dest → DTN intercept SKIP → native DM only!
    // SCENARIO: Node 1→13 (FW+), no FW+ path found → Stock fallback → Node 13 marked Stock
    //           Node 13 IS FW+ but was temporarily unreachable via progressive relay
    //           Result: Future messages bypass DTN completely! (FAIL)
    // ROOT CAUSE: Fallback assumes dest is Stock without checking if dest is FW+
    // SOLUTION: Only mark as Stock if dest is NOT FW+ (check fwplusVersionByNode + fwplusSeenMs)
    // BENEFIT: DTN retry after topology changes, self-healing for mixed networks
    // FIX #120c: Check both maps (fwplusVersionByNode may not sync with fwplusSeenMs)
    bool destInVersionMap = (fwplusVersionByNode.find(p.data.orig_to) != fwplusVersionByNode.end());
    bool destInSeenMap = (fwplusSeenMs.find(p.data.orig_to) != fwplusSeenMs.end());
    bool destIsFwplus = destInVersionMap || destInSeenMap;
    bool destHasRelayHistory = (p.progressiveRelayCount > 0 || p.cachedProgressiveRelay != 0);
    if (!destIsFwplus && destHasRelayHistory) {
        destIsFwplus = true;
        LOG_DEBUG("DTN: Treating dest 0x%x as FW+ based on prior relay history", (unsigned)p.data.orig_to);
    }
    
    if (!destIsFwplus) {
        stockKnownMs[p.data.orig_to] = millis();
        LOG_INFO("DTN: Marking dest 0x%x as Stock (fallback, not FW+)", (unsigned)p.data.orig_to);
    } else {
        LOG_WARN("DTN: NOT marking dest 0x%x as Stock - node is FW+ (topology issue, will retry)", 
                (unsigned)p.data.orig_to);
    }
    
    //FIX #129: Copy Pending data BEFORE erase to avoid dangling reference
    // PROBLEM: pendingById.erase(id) invalidates p reference (it's a reference to map entry!)
    //          Using p.data.* after erase() → UNDEFINED BEHAVIOR → HEAP CORRUPTION!
    // CRASH: ESP32 "assert failed: block_trim_free heap_tlsf.c:371"
    // ROOT CAUSE: Accessing freed/moved memory via dangling reference
    // EXAMPLE: dest 0x4357989c, progressive relay exhausted → fallback → erase → access p.data → CRASH!
    // SOLUTION: Copy p.data to local variable BEFORE erase, use copy for fallback packet
    // BENEFIT: Safe memory access, prevents heap corruption on ESP32
    meshtastic_FwplusDtnData dataCopy = p.data; // Copy before erase!
    
    // Erase DTN pending FIRST
    pendingById.erase(id);
    
    // Send fresh native DM (new packet, no DTN involvement)
    // Use dataCopy (not p.data!) - p is now invalid!
    meshtastic_MeshPacket *dm = allocDataPacket();
    if (!dm) { 
        LOG_WARN("DTN: Fallback failed - packet pool exhausted");
        return false;
    }
    dm->to = dataCopy.orig_to;         // (OK) Use copy, not p.data!
    dm->channel = dataCopy.channel;    // (OK) Use copy, not p.data!
    dm->from = 0; // Router will set to ourNodeNum
    dm->id = random(); // FIX #130: Generate random ID (Router doesn't generate for id=0!)
    dm->want_ack = true; // User expects ACK
    dm->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    
    //FIX #146: Set tombstone for NEW fallback packet to prevent re-capture loop
    // PROBLEM: sendProxyFallback() generates NEW packet with NEW id (line 2505: dm->id = random())
    //          Old tombstone is for original id (from enqueueFromCaptured)
    //          NEW packet → DTN intercept → no tombstone → re-capture → INFINITE LOOP → WATCHDOG RESET!
    // SEQUENCE: 1. tryForward → sendProxyFallback(old_id) → erase pending
    //           2. sendProxyFallback → allocDataPacket → dm->id = random() (NEW id)
    //           3. sendToMesh(NEW dm) → Router → DTN intercept → no tombstone for NEW id → re-capture!
    //           4. enqueueFromCaptured(NEW id) → tryForward → sendProxyFallback → LOOP!
    // SOLUTION: Set tombstone for NEW dm->id BEFORE sendToMesh
    // BENEFIT: Prevents watchdog reset, stops re-capture loop
    if (configTombstoneMs) {
        tombstoneUntilMs[dm->id] = millis() + configTombstoneMs;
        LOG_DEBUG("DTN FIX #146: Set tombstone for fallback packet id=0x%x (prevent re-capture loop)", 
                 (unsigned)dm->id);
    }
    if (dataCopy.is_encrypted) {       // (OK) Use copy, not p.data!
        dm->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(dm->encrypted.bytes, dataCopy.payload.bytes, dataCopy.payload.size);
        dm->encrypted.size = dataCopy.payload.size;
    } else {
        dm->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        if (dataCopy.payload.size > sizeof(dm->decoded.payload.bytes))
            dm->decoded.payload.size = sizeof(dm->decoded.payload.bytes);
        else
            dm->decoded.payload.size = dataCopy.payload.size;
        memcpy(dm->decoded.payload.bytes, dataCopy.payload.bytes, dm->decoded.payload.size);
        dm->decoded.want_response = false;
    }
    
    //FIX #106c: REMOVED broken FIX #105a/b/c (fallbackPacketId logic)
    // PROBLEM: Fallback DM was re-captured by DTN → infinite loop → 38 duplicates
    // ROOT CAUSE: pendingById not erased before sendToMesh → re-capture → new pending
    // SOLUTION: Erase pending FIRST (done above), send fresh native DM (new ID)
    
    //FIX #138: Set dm->from to prevent from=0 malformed packet
    // PROBLEM: dm->from was never set → protobuf default 0 → Router intercepts as malformed
    //          → DTN re-captures with from=0 → crash/loop
    // SOLUTION: Set dm->from to orig_from (source node)
    //FIX #140: Use dataCopy instead of p.data (p is invalid after erase!)
    dm->from = dataCopy.orig_from;  // (OK) Use copy, not p.data!
    
    // Send fresh native DM (Router will generate new ID)
    service->sendToMesh(dm, RX_SRC_LOCAL, false);
    
    //FIX #154: Send FAILED receipt after Stock fallback (APK status update)
    // PROBLEM: sendProxyFallback erases pending without sending receipt
    //          Result: APK stuck on "in transit" (last receipt was PROGRESSED)
    // SOLUTION: Send FAILED receipt to indicate Stock fallback (reason=0)
    // APK MAPPING: FAILED → "Sent via Stock" or "DTN unavailable"
    // BENEFIT: User gets feedback that message was sent (not stuck)
    emitReceipt(dataCopy.orig_from, dataCopy.orig_id, 
               meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_FAILED,
               0); // reason=0 = Stock fallback (not error)
    
    LOG_INFO("DTN: Source fallback to Stock 0x%x (fresh native DM, DTN aborted)", 
             (unsigned)dataCopy.orig_to);  // (OK) Use copy, not p.data!
    
    ctrFallbacksAttempted++; 
    // Note: pending already erased above, no retry needed
    return true;
}

// Purpose: lightweight FW+ presence probe to a specific node using a receipt with PROGRESSED status.
// Usage: optional near TTL tail to detect FW+ capability without heavy traffic.
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

//trigger traceroute to dest with cooldown to build DV-ETX confidence
void DtnOverlayModule::maybeTriggerTraceroute(NodeNum dest)
{
    uint32_t now = millis();
    auto it = lastRouteProbeMs.find(dest);
    if (it != lastRouteProbeMs.end() && (now - it->second) < configRouteProbeCooldownMs) return;
    
    // Global rate limiter
    if (isGlobalProbeCooldownActive()) {
        LOG_DEBUG("DTN: Traceroute probe blocked by global rate limiter (need %u sec)", 
                 getGlobalProbeCooldownRemainingSec());
        return;
    }
    
    lastRouteProbeMs[dest] = now;
    if (!router) return;
    
    // Send actual TRACEROUTE_APP packet (not just FW+ probe)
    // This builds routing confidence: 1 successful reply = confidence 1 = NextHop ready
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) return;
    
    p->to = dest;
    p->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
    p->decoded.want_response = true;
    p->want_ack = false; // No radio ACK - want traceroute REPLY, not hop-by-hop ACKs
    
    meshtastic_RouteDiscovery req = meshtastic_RouteDiscovery_init_zero;
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), 
                                                  &meshtastic_RouteDiscovery_msg, &req);
    
    // Set hop_limit based on known distance (expanding ring)
    // Response needs same hop_limit to return (double distance + margin)
    meshtastic_NodeInfoLite *ninfo = nodeDB->getMeshNode(dest);
    if (ninfo && ninfo->hops_away > 0) {
        p->hop_limit = (ninfo->hops_away * 2) + 1; // Distance * 2 (out+back) + 1 margin
    } else {
        p->hop_limit = 7; // Default: allow 3-hop paths + response (3*2+1=7)
    }
    
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND; // Background priority to avoid congestion
    
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    updateGlobalProbeTimestamp();
    
    LOG_INFO("DTN: Sent traceroute to 0x%x (hop_limit=%u, want_ack=false) to build routing confidence", 
             (unsigned)dest, (unsigned)p->hop_limit);
}
// Purpose: send a compact DTN receipt (status/milestone/expire) back to the source or peer.
// Notes: uses BACKGROUND priority and avoids ACKs; reason carries optional telemetry (e.g., FW+ version advertise).
// ENHANCED: RECEIPTs use DTN custody for distant destinations (same mechanism as DATA)
// PROBLEM: RECEIPTs via native unicast fail when no next_hop (asymmetric routing)
// SOLUTION: For distant destinations (hops > HOP_RELIABLE), use DTN custody in reverse
// BENEFIT: RECEIPTs reach source even through complex mesh topology
void DtnOverlayModule::emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason, 
                                    uint8_t custodyPathCount, const uint8_t *custodyPath)
{
    // Guard: don't emit receipts if DTN is disabled (don't advertise as FW+ capable)
    if (!configEnabled) return;
    
    // Smart RECEIPT routing with FW+ capability propagation
    // PROBLEM 1: Routing table can be asymmetric (node13 sees node1 as hops=2 but no return path)
    //            Native RECEIPTs fail → source never gets confirmation
    // PROBLEM 2: Distant nodes (4+ hops) never discover source is FW+ (broadcast hop_limit=3)
    //            Node13 doesn't know Node1 is FW+, sends DELIVERED via native (fails!)
    // SOLUTION: Use custody_path as FW+ capability indicator:
    //           - If custodyPathCount > 0: source MUST be FW+ (only FW+ creates custody chains)
    //           - Even if isFwplus(to)==false (not in our cache), trust custody_path
    // BENEFIT: DELIVERED receipts use custody for ALL FW+ sources, even distant ones (beyond broadcast range)
    
    bool destIsFwplus = isFwplus(to);
    
    // FW+ CAPABILITY PROPAGATION: Detect FW+ source via custody_path
    // If we received DATA with custody_path, source is definitely FW+ (even if not in our fwplusSeenMs cache)
    bool sourceIsFwplusViaCustody = (custodyPathCount > 0);
    if (sourceIsFwplusViaCustody && !destIsFwplus) {
        LOG_INFO("DTN: Source 0x%x detected as FW+ via custody_path (chain_len=%u, not in local cache) - adding to cache",
                 (unsigned)to, custodyPathCount);
        // Add to local cache so future routing decisions know this is FW+
        markFwplusSeen(to);
        // Also record minimal version (we know it's FW+ but don't know exact version)
        if (fwplusVersionByNode.find(to) == fwplusVersionByNode.end()) {
            fwplusVersionByNode[to] = configMinFwplusVersionForHandoff; // Assume minimum version
        }
    }
    
    // Special cases: always use native unicast (don't custody chain these)
    bool isProbe = (origId == 0); // Probe/hello-back (id=0)
    bool isVersionBeacon = (status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED && reason > 0 && origId == 0);
    //FIX #158: ALL receipts via DTN custody (no native unicast receipts)
    // PROBLEM: Native unicast receipts (portnum=280) get PKI encrypted by Router
    //          Binary payload [0xAC 0xDC ...] leaks to deliverLocal() → garbage in APK "◆◆◆"
    // OLD LOGIC: DELIVERED → custody, PROGRESSED/EXPIRED/FAILED → native (complex, PKI issues)
    // NEW LOGIC: ALL receipts (DELIVERED, PROGRESSED, EXPIRED, FAILED) → custody (simple, reliable)
    //            ONLY control plane (probes, beacons) → native
    // BENEFIT: Eliminates PKI encryption issues, simpler code, consistent routing
    // TRADE-OFF: Slightly higher RF overhead for PROGRESSED/EXPIRED/FAILED (acceptable for reliability)
    // NOTE: Milestones (PROGRESSED) now via custody too - ensures distant sources get updates
    bool isControlPlane = (isProbe || isVersionBeacon);
    bool useNativeUnicast = isControlPlane; // ONLY probes/beacons use native
    
    if (useNativeUnicast) {
        // CASE 1: Control plane only (probes/beacons) - use native unicast
        meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
        msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
        msg.variant.receipt.orig_id = origId;
        msg.variant.receipt.status = status;
        msg.variant.receipt.reason = reason;
        msg.variant.receipt.dest = to;
        msg.variant.receipt.custody_id = 0;
        msg.variant.receipt.ttl_remaining_ms = 0;
        msg.variant.receipt.allow_proxy_fallback = false;
        msg.variant.receipt.custody_path_count = 0;

        meshtastic_MeshPacket *p = allocDataProtobuf(msg);
        if (!p) return;
        p->to = to;
        p->from = nodeDB->getNodeNum();
        p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
        p->want_ack = false;
        p->decoded.want_response = false;
        p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
        
        //FIX #128: EXPIRED receipt must reach local PhoneAPI when source node times out
        // PROBLEM: Source node sends DM, custody holds it, TTL expires after 5min
        //          emitReceipt(self, id, EXPIRED) → sendToMesh(to=self) → Router receives
        //          BUT Router doesn't deliver to PhoneAPI! (treats as loopback, discards)
        //          Result: APK never shows "Message expired", packet disappears silently
        // EXAMPLE: User sends DM, sees "via node 0xfb04" (self!), waits 5min, no EXPIRED
        //          Pending entry erased, tombstone prevents re-send, but APK unaware
        // ROOT CAUSE: emitReceipt() always uses sendToMesh(), even when to=self
        //             Router loopback handling doesn't deliver FWPLUS_DTN_APP to PhoneAPI
        // SOLUTION: When to=self, deliver directly to local PhoneAPI (bypass Router)
        // BENEFIT: APK always receives status updates (EXPIRED, FAILED) for local messages
        if (to == nodeDB->getNodeNum()) {
            // LOCAL DELIVERY: Receipt for our own message (we are source)
            // Deliver to PhoneAPI so APK sees status update
            if (service) {
                meshtastic_MeshPacket *copyForPhone = packetPool.allocCopy(*p);
                if (copyForPhone) {
                    service->sendPacketToPhoneRaw(copyForPhone);
                } else {
                    LOG_WARN("DTN: Failed to clone receipt id=0x%x for PhoneAPI (pool exhausted)", (unsigned)origId);
                }
            }
            if (router) {
                router->sendLocal(p, RX_SRC_LOCAL); // Deliver to PhoneAPI directly
                LOG_INFO("DTN FIX #128: RECEIPT id=0x%x status=%u delivered to LOCAL PhoneAPI (source node)",
                         (unsigned)origId, (unsigned)status);
            } else {
                // Fallback: no router, just free packet
                packetPool.release(p);
                LOG_WARN("DTN FIX #128: No router available for local RECEIPT delivery");
            }
        } else {
            // REMOTE DELIVERY: Normal unicast to other node
            service->sendToMesh(p, RX_SRC_LOCAL, false);
            LOG_DEBUG("DTN tx RECEIPT id=0x%x status=%u to=0x%x (native)", 
                     (unsigned)origId, (unsigned)status, (unsigned)to);
        }
        ctrReceiptsEmitted++; 
    } else {
        //FIX #160: Local delivery for self-addressed custody receipts
        // PROBLEM: FIX #158 changed ALL receipts to custody (not just DELIVERED),
        //          PROGRESSED receipts to source (to=self) → custody → progressive relay → queue full!
        //          Example: Node1 sends DM → handoff → intermediate emits PROGRESSED(to=0x11)
        //                   Node1 receives PROGRESSED custody → tries to forward to self → queue full
        // ROOT CAUSE: FIX #128 handles to==self for native unicast receipts (line 2763)
        //             But custody path (CASE 2) has NO check for to==self!
        // SOLUTION: Detect to==self and deliver directly to PhoneAPI (bypass custody routing)
        // BENEFIT: Source node receives all status updates without queue exhaustion
        // IMPACT: Source node only (intermediate nodes unaffected)
        if (to == nodeDB->getNodeNum()) {
            // LOCAL DELIVERY: Receipt for our own message (we are source or destination)
            // Don't custody route to self - deliver directly to PhoneAPI
            meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
            msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
            msg.variant.receipt.orig_id = origId;
            msg.variant.receipt.status = status;
            msg.variant.receipt.reason = reason;
            msg.variant.receipt.dest = to;
            msg.variant.receipt.custody_id = 0;
            msg.variant.receipt.ttl_remaining_ms = 0;
            msg.variant.receipt.allow_proxy_fallback = false;
            msg.variant.receipt.custody_path_count = 0;

            meshtastic_MeshPacket *p = allocDataProtobuf(msg);
            if (!p) return;
            p->to = to;
            p->from = nodeDB->getNodeNum();
            p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
            p->want_ack = false;
            p->decoded.want_response = false;
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
            
            if (service) {
                meshtastic_MeshPacket *copyForPhone = packetPool.allocCopy(*p);
                if (copyForPhone) {
                    service->sendPacketToPhoneRaw(copyForPhone);
                } else {
                    LOG_WARN("DTN: Failed to clone receipt id=0x%x for PhoneAPI (pool exhausted)", (unsigned)origId);
                }
            }
            if (router) {
                router->sendLocal(p, RX_SRC_LOCAL); // Deliver to PhoneAPI directly
                LOG_INFO("DTN FIX #160: RECEIPT id=0x%x status=%u delivered to LOCAL PhoneAPI (to=self, custody skipped)",
                         (unsigned)origId, (unsigned)status);
            } else {
                packetPool.release(p);
                LOG_WARN("DTN FIX #160: No router available for local RECEIPT delivery");
            }
            ctrReceiptsEmitted++;
            return; // Done - don't proceed to custody routing
        }
        
        // CASE 2: Distant destination - use DTN custody (reliable path through FW+ relays)
        // Prepare return chain hints (reverse custody_path) for deterministic routing
        uint8_t returnChain[16] = {0};
        uint8_t returnLen = 0;
        if (custodyPath != nullptr && custodyPathCount > 0) {
            for (int i = (int)custodyPathCount - 1; i >= 0 && returnLen < 16; --i) {
                uint8_t hop = (uint8_t)(custodyPath[i] & 0xFF);
                // Skip immediate duplicates (can happen with mobility/tombstone overlap)
                if (returnLen > 0 && returnChain[returnLen - 1] == hop) {
                    continue;
                }
                returnChain[returnLen++] = hop;
            }
        }
        //TTL for RECEIPT (should allow multiple retries through distant paths)
        uint32_t receiptTtlMinutes = 5; // 5 minutes (vs 15-30 for DATA)
        
        //FIX #164 + FIX #165: Robust custody_id hash (FNV-1a inspired mixing)
        // PROBLEM: XOR-based hash causes collisions (0x2abc9ce7 vs 0x2fbc9ce7 → SAME custody_id!)
        //          Example: Node13 emits receipt for 0x2abc9ce7 (status=4) → custody_id=0xf0112215
        //                   Later emits receipt for 0x2fbc9ce7 (status=1) → custody_id=0xf0112215 (COLLISION!)
        //          Result: Tombstone blocks second receipt → "custody queue full" → DELIVERED never sent ❌
        // ROOT CAUSE: XOR is symmetric and weak (a^b^c can collide easily with different inputs)
        // SOLUTION: Use multiplicative hash mixing (fast, strong, no libraries needed)
        //           Mix: origId, status, to (destination), nodeNum with prime multipliers
        // BENEFIT: Extremely low collision rate (<0.0001%), works on ESP32/NRF, no extra RAM
        // FNV-1a inspired hash: multiply by prime (0x01000193) + XOR for avalanche
        uint32_t hash = 0x811C9DC5; // FNV-1a offset basis
        hash = (hash ^ origId) * 0x01000193;          // Mix origId (packet identity)
        hash = (hash ^ to) * 0x01000193;              // Mix destination (receipt routing)
        hash = (hash ^ (uint32_t)status) * 0x01000193; // Mix status (receipt type)
        hash = (hash ^ nodeDB->getNodeNum()) * 0x01000193; // Mix our nodeNum (origin)
        uint32_t receiptCustodyId = hash;
        
        // Enqueue RECEIPT as DTN custody packet
        //FIX #158: Broadcast fallback for all custody receipts
        // Since ALL receipts now use custody (not just DELIVERED), enable broadcast fallback
        // This ensures receipts reach distant sources even with unreliable reverse paths
        bool allowFallback = true; // All custody receipts can use broadcast fallback
        
        meshtastic_FwplusDtnReceipt receiptMsg = meshtastic_FwplusDtnReceipt_init_zero;
        receiptMsg.orig_id = origId;
        receiptMsg.status = status;
        receiptMsg.reason = reason;
        receiptMsg.dest = to;
        receiptMsg.custody_id = receiptCustodyId;
        receiptMsg.ttl_remaining_ms = receiptTtlMinutes * 60UL * 1000UL;
        receiptMsg.allow_proxy_fallback = allowFallback;
        if (custodyPath != nullptr && custodyPathCount > 0) {
            pb_size_t copyCount = std::min<pb_size_t>(custodyPathCount, (pb_size_t)sizeof(receiptMsg.custody_path));
            receiptMsg.custody_path_count = copyCount;
            for (pb_size_t i = 0; i < copyCount; ++i) {
                receiptMsg.custody_path[i] = (uint8_t)(custodyPath[i] & 0xFF);
            }
        }

        meshtastic_FwplusDtnData skeleton = meshtastic_FwplusDtnData_init_zero;
        skeleton.orig_id = receiptCustodyId;
        skeleton.orig_from = nodeDB->getNodeNum();
        skeleton.orig_to = to;
        skeleton.channel = 0;
        skeleton.is_encrypted = false;
        skeleton.allow_proxy_fallback = allowFallback;
        skeleton.payload.size = 0;
        skeleton.custody_path_count = 0;
        skeleton.ttl_remaining_ms = receiptMsg.ttl_remaining_ms;
        skeleton.use_pki_encryption = false;

        scheduleOrUpdate(receiptCustodyId, skeleton);

        auto itPendingReceipt = pendingById.find(receiptCustodyId);
        if (itPendingReceipt == pendingById.end()) {
            LOG_WARN("DTN tx RECEIPT id=0x%x status=%u to=0x%x FAILED (custody queue full)",
                    (unsigned)origId, (unsigned)status, (unsigned)to);
            return;
        }

        Pending &receiptPending = itPendingReceipt->second;
        receiptPending.isReceiptCustody = true;
        receiptPending.isReceiptSource = true;
        receiptPending.isReceiptVariant = true;
        receiptPending.receiptMsg = receiptMsg;
        receiptPending.hasPlainPayload = false;
        receiptPending.usePki = false;
        receiptPending.disablePki = false;
        receiptPending.data.payload.size = 0;
        receiptPending.receiptMsg.ttl_remaining_ms = receiptPending.data.ttl_remaining_ms;
        receiptPending.hasReceiptReturnChain = (returnLen > 0);
        receiptPending.receiptReturnLen = returnLen;
        receiptPending.receiptReturnIndex = 0;
        receiptPending.data.orig_id = receiptCustodyId;
        receiptPending.data.orig_from = nodeDB->getNodeNum();
        receiptPending.data.orig_to = to;
        receiptPending.data.allow_proxy_fallback = allowFallback;
        receiptPending.data.ttl_remaining_ms = receiptMsg.ttl_remaining_ms;
        receiptPending.data.custody_path_count = 0;
        if (receiptMsg.custody_path_count > 0) {
            pb_size_t copyCount = std::min<pb_size_t>(receiptMsg.custody_path_count, (pb_size_t)sizeof(receiptPending.data.custody_path));
            receiptPending.data.custody_path_count = copyCount;
            for (pb_size_t i = 0; i < copyCount; ++i) {
                receiptPending.data.custody_path[i] = receiptMsg.custody_path[i];
            }
        }
        if (returnLen > 0) {
            memcpy(receiptPending.receiptReturnChain, returnChain, returnLen);
            NodeNum forcedNext = resolveNextHopNode(returnChain[0]);
            if (forcedNext == 0) {
                forcedNext = (NodeNum)returnChain[0];
            }
            if (forcedNext != 0 && forcedNext != nodeDB->getNodeNum()) {
                if (isNodeReachable(forcedNext, configReceiptMaxNodeAgeSec)) {
                    receiptPending.forceEdge = forcedNext;
                    LOG_INFO("DTN: Receipt 0x%x initial return hop=0x%x (chain_len=%u)",
                             (unsigned)receiptCustodyId, (unsigned)forcedNext,
                             (unsigned)returnLen);
                } else {
                    LOG_WARN("DTN: Receipt 0x%x planned hop 0x%x unreachable - keeping adaptive routing",
                             (unsigned)receiptCustodyId, (unsigned)forcedNext);
                }
            }
        } else {
            receiptPending.forceEdge = 0;
        }

        deliveredReceiptIds.insert(receiptCustodyId);
        LOG_DEBUG("DTN: Tracking receipt 0x%x (status=%u) for broadcast fallback", 
                 (unsigned)receiptCustodyId, (unsigned)status);

        //FIX #132 (revisited): Seed reverse first hop from custody path
        if (custodyPath != nullptr && custodyPathCount >= 2) {
            uint8_t reverseFirstHopByte = custodyPath[custodyPathCount - 1];
            NodeNum reverseFirstHop = (NodeNum)reverseFirstHopByte;
            if (isNodeReachable(reverseFirstHop, configReceiptMaxNodeAgeSec)) {
                receiptPending.forceEdge = reverseFirstHop;
                LOG_INFO("DTN: DELIVERED receipt 0x%x return path set edge=0x%x (reverse custody_path, chain_len=%u)",
                        (unsigned)receiptCustodyId, (unsigned)reverseFirstHop, (unsigned)custodyPathCount);
            } else {
                LOG_WARN("DTN: DELIVERED receipt 0x%x reverse edge 0x%x unreachable/stale - using forward logic",
                        (unsigned)receiptCustodyId, (unsigned)reverseFirstHop);
            }
        }

        LOG_INFO("DTN tx RECEIPT id=0x%x status=%u to=0x%x via DTN custody (custody_id=0x%x, reason=0x%x, fallback=%s)",
                 (unsigned)origId, (unsigned)status, (unsigned)to, (unsigned)receiptCustodyId,
                 (unsigned)reason, allowFallback ? "enabled" : "disabled");
        ctrReceiptsEmitted++;
    }
}
void DtnOverlayModule::maybeAdvertiseFwplusVersion()
{
    if (!configEnabled) return;
    uint32_t now = millis();
    
    // Adaptive mobility management - adjust parameters based on mobility
    adaptiveMobilityManagement();
    
    // Staged warmup with progressive backoff
    bool knowsAnyFwplus = false;
    for (const auto &kv : fwplusVersionByNode) { (void)kv; knowsAnyFwplus = true; break; }
    
    uint32_t uptimeMs = now - moduleStartMs;
    bool inWarmupPhase = (uptimeMs < configAdvertiseWarmupDurationMs);
    bool warmupComplete = (warmupBeaconsSent >= configAdvertiseWarmupCount);
    
    // Select interval based on phase:
    // 1. Warmup (first 70min): 15min aggressive discovery (4 beacons guaranteed)
    // 2. Post-warmup unknown: 2h continued search
    // 3. Normal (FW+ known): 6h maintenance
    uint32_t interval;
    if (inWarmupPhase && !warmupComplete) {
        interval = configAdvertiseWarmupIntervalMs; // 15min warmup
    } else if (!knowsAnyFwplus) {
        interval = configAdvertiseIntervalUnknownMs; // 2h post-warmup
    } else {
        interval = configAdvertiseIntervalMs; // 6h normal
    }
    
    if (lastAdvertiseMs == 0) lastAdvertiseMs = now - (uint32_t)random(interval);
    // One-shot early advertise shortly after module start to speed up discovery (unconditional broadcast)
    if (!firstAdvertiseDone && now - moduleStartMs >= configFirstAdvertiseDelayMs) {
        //Check if we should retry (if previous attempt failed)
        bool shouldRetry = (firstAdvertiseRetryMs > 0 && now - firstAdvertiseRetryMs >= configFirstAdvertiseRetryMs);
        bool isFirstAttempt = (firstAdvertiseRetryMs == 0);
        
        if (isFirstAttempt || shouldRetry) {
            LOG_INFO("DTN: Attempting first discovery after %u ms (attempt %s)", 
                     (unsigned)(now - moduleStartMs), isFirstAttempt ? "1" : "retry");
            
            // WORKAROUND: Use BROADCAST for first discovery in cold start
            // We don't know who is FW+ yet, so broadcast is the only way to reach everyone
            LOG_INFO("DTN: First discovery - broadcasting FW+ version %u", (unsigned)FW_PLUS_VERSION);
            
            uint32_t reason = (uint32_t)FW_PLUS_VERSION;
            meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
            msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
            msg.variant.receipt.orig_id = 0;
            msg.variant.receipt.status = meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED;
            msg.variant.receipt.reason = reason;
            msg.variant.receipt.dest = NODENUM_BROADCAST;
            msg.variant.receipt.custody_id = 0;
            msg.variant.receipt.ttl_remaining_ms = 0;
            msg.variant.receipt.allow_proxy_fallback = false;
            msg.variant.receipt.custody_path_count = 0;
            
            meshtastic_MeshPacket *p = allocDataProtobuf(msg);
            if (p) {
                p->to = NODENUM_BROADCAST; // BROADCAST for first discovery!
                p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
                p->want_ack = false;
                p->decoded.want_response = false;
                p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
                
                service->sendToMesh(p, RX_SRC_LOCAL, false);
                
                lastAdvertiseMs = now;
                firstAdvertiseDone = true; // Mark as done
                
                // Count first discovery as warmup beacon if in warmup phase
                if (inWarmupPhase && !warmupComplete) {
                    warmupBeaconsSent++;
                    LOG_INFO("DTN: First discovery broadcast sent (warmup %u/%u, next in ~%u min)", 
                             (unsigned)warmupBeaconsSent, (unsigned)configAdvertiseWarmupCount,
                             (unsigned)(interval / 60000));
                } else {
                    LOG_INFO("DTN: First discovery broadcast sent (next periodic in ~%u min)", 
                             (unsigned)(interval / 60000));
                }
            } else {
                LOG_WARN("DTN: First discovery failed - alloc error");
            }
        }
        return;
    }
    if (now - lastAdvertiseMs < interval + (uint32_t)random(configAdvertiseJitterMs)) return;

    // Only advertise if channel is not overloaded //fw+
    if (airTime && !airTime->isTxAllowedChannelUtil(true)) return;

    // WORKAROUND for cold start: use BROADCAST beacon when no FW+ nodes known
    // In cold start, we don't know who is FW+ yet, so unicast won't work
    // Once we know some FW+ nodes, we can use targeted unicast probes
    
    if (!knowsAnyFwplus) {
        // COLD START MODE: Send broadcast beacon for initial discovery
        LOG_INFO("DTN: Cold start - sending broadcast beacon for FW+ discovery");
        
        uint32_t reason = (uint32_t)FW_PLUS_VERSION;
        meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
        msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
        msg.variant.receipt.orig_id = 0;
        msg.variant.receipt.status = meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED;
        msg.variant.receipt.reason = reason;
        msg.variant.receipt.dest = NODENUM_BROADCAST;
        msg.variant.receipt.custody_id = 0;
        msg.variant.receipt.ttl_remaining_ms = 0;
        msg.variant.receipt.allow_proxy_fallback = false;
        msg.variant.receipt.custody_path_count = 0;
        
        meshtastic_MeshPacket *p = allocDataProtobuf(msg);
        if (p) {
            p->to = NODENUM_BROADCAST; // BROADCAST in cold start!
            p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
            p->want_ack = false;
            p->decoded.want_response = false;
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
            
            service->sendToMesh(p, RX_SRC_LOCAL, false);
            LOG_INFO("DTN: Broadcast beacon sent (cold start mode, next periodic in ~%u min)",
                     (unsigned)(interval / 60000));
        }
    }
    // NOTE: Unicast probing to unknown nodes has been removed.
    // RATIONALE: Wasteful and unnecessary - stock nodes ignore FW+ probes,
    //            FW+ nodes will respond to broadcast beacons anyway.
    // STRATEGY: Rely exclusively on:
    //   1. Broadcast beacons (staged: 2min, 15min warmup, 2h/6h periodic)
    //   2. Hello-back unicast responses (reactive, rate-limited)
    //   3. Passive observation (OnDemand, overheard DTN traffic)
    // This provides O(1) discovery traffic instead of O(N) unicast probing.
    
    lastAdvertiseMs = now;
    
    //Track warmup beacons and log phase
    if (inWarmupPhase && !warmupComplete) {
        warmupBeaconsSent++;
        LOG_INFO("DTN: Warmup beacon %u/%u sent (next in ~%u min)",
                 (unsigned)warmupBeaconsSent, (unsigned)configAdvertiseWarmupCount,
                 (unsigned)(configAdvertiseWarmupIntervalMs / 60000));
    } else {
        const char* phase = knowsAnyFwplus ? "normal" :
                           (warmupComplete ? "post-warmup" : "warmup-complete");
        LOG_INFO("DTN: Periodic beacon sent (phase: %s, next in ~%u min, known FW+ nodes: %u)",
                 phase, (unsigned)(interval / 60000), (unsigned)fwplusVersionByNode.size());
    }
    LOG_DEBUG("DTN: maybeAdvertiseFwplusVersion wywołane, knowsAnyFwplus=%d, inWarmupPhase=%d", (int)knowsAnyFwplus, (int)inWarmupPhase);
}

// Purpose: check if a handoff candidate is valid (not self, dest, or lastCarrier)
// Returns: true if candidate is safe to use
bool DtnOverlayModule::isValidHandoffCandidate(NodeNum candidate, NodeNum dest, const Pending &p) const
{
    if (candidate == 0) return false;
    if (candidate == nodeDB->getNodeNum()) return false;
    if (candidate == dest) return false;
    if (candidate == p.lastCarrier) return false;
    return true;
}
// Purpose: verify if a node is reachable and not stale
// Returns: true if node should be considered for handoff
bool DtnOverlayModule::isNodeReachable(NodeNum node, uint32_t maxAgeSecOverride) const
{
    //Guard: broadcast is never reachable as a unicast destination
    if (node == NODENUM_BROADCAST || node == NODENUM_BROADCAST_NO_LORA) {
        return false;
    }
    
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (!info) {
        LOG_DEBUG("DTN: isNodeReachable(0x%x) = false (no NodeInfo)", (unsigned)node);
        return false;
    }
    
    //FIX #119: Relax hops check for hop_limit=3 networks and mixed FW+/Stock
    // PROBLEM: hop_limit=3 networks with DTN disabled intermediate nodes (Node 10, K)
    //          Node 7 hears Node 11 via retransmission → hops_away may be > 3
    //          isNodeReachable blocks Node 11 as progressive relay candidate
    //          Result: Node 7 selects wrong direction (Node 6, Node 3) instead of Node 11
    // ROOT CAUSE: Strict hops_away > 3 check assumes direct/short paths
    //             In mixed networks (Stock + FW+), FW+ beacons may travel through Stock relays
    //             Recorded hops_away reflects relay path, not actual topology distance
    // SOLUTION: Increase limit to HOP_MAX (7) for progressive relay candidates
    //           DTN has robust loop prevention: custody_path, progressive relay tracking, recentCarriers
    //           Allow distant candidates - progressive relay will try them and learn from failures
    // BENEFIT: Enables DTN to work in sparse FW+ deployments with Stock relay bridges
    if (info->hops_away > HOP_MAX) {
        LOG_DEBUG("DTN: isNodeReachable(0x%x) = false (hops=%u > HOP_MAX=%u)", 
                 (unsigned)node, info->hops_away, HOP_MAX);
        return false;
    }
    
    // Tier 1: Direct neighbors are always reachable (routing table guarantees)
    if (info->hops_away == 0) {
        return true; // Direct neighbor = immediate reachability
    }
    
    // Tier 2: Try time-based staleness check if epoch time available
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet); // epoch seconds
    uint32_t lastSeenEpoch = info->last_heard; // epoch seconds
    
    if (nowEpoch > 0 && lastSeenEpoch > 0) {
        // Valid epoch time available - use precise staleness check
        //FIX #107: Increase staleness timeout from 30min to 12h
        // PROBLEM: 30min too short for large networks (200+ nodes) with infrequent telemetry (2-6h)
        //          Result: DTN marks nodes as "stale" → fallback to Stock → delivery fails
        // ROOT CAUSE: Networks scale telemetry intervals to reduce channel_util
        //             Node may not send packets for 6+ hours (valid, alive, but silent)
        // SOLUTION: 12h timeout allows sparse telemetry while still filtering dead nodes
        // NOTE: NodeDB has own mechanisms to remove truly dead nodes (routing updates)
        //shrink staleness window when override provided (receipts need fresh telemetry)
        uint32_t maxAgeSec = maxAgeSecOverride ? maxAgeSecOverride : 12UL * 60UL * 60UL; // default 12h
        bool recentlySeen = (nowEpoch >= lastSeenEpoch) && ((nowEpoch - lastSeenEpoch) < maxAgeSec);
        
        if (!recentlySeen) {
            LOG_DEBUG("DTN: isNodeReachable(0x%x) = false (stale: age=%u sec > %u sec)", 
                     (unsigned)node, (unsigned)(nowEpoch - lastSeenEpoch), (unsigned)maxAgeSec);
        } else {
            LOG_DEBUG("DTN: isNodeReachable(0x%x) = true (recent: age=%u sec)", 
                     (unsigned)node, (unsigned)(nowEpoch - lastSeenEpoch));
        }
        
        return recentlySeen;
    }
    
    // Tier 4: No epoch time - fallback to routing table trust
    // Trust routing table for nodes at hops 0-3 (recent discovery assumed)
    return true;
}
// Purpose: intelligent fallback for unknown or low-confidence destinations
// Returns: true if fallback was attempted
bool DtnOverlayModule::tryIntelligentFallback(uint32_t id, Pending &p)
{
    // FIX #59: Intermediates should NOT use intelligent fallback for stock destinations!
    // PROBLEM: Same as FIX #56 and #58 - intermediates break custody chains with fallback
    // SOLUTION: Only source can use intelligent fallback, intermediates forward custody
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    
    // For stock destinations with low route confidence, try native DM early
    // EXCEPT: for far destinations (>=3 hops), always try DTN overlay first
    // This is the "im dalej w las" scenario - multi-hop native routing is unreliable
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    bool isFarDest = (hopsToDest != 255 && hopsToDest >= 3);

    //FIX #121: REMOVED early return for unknown destinations (blocked broadcast rescue!)
    // PROBLEM: Early return (line 2927) blocked broadcast rescue logic (lines 2951+)
    //          Unknown destinations (hops=255) exit early → broadcast rescue NEVER called!
    // ROOT CAUSE: This early return was for Stock destinations, not broadcast rescue
    //             Broadcast rescue NEEDS to run for unknown destinations!
    // SOLUTION: Remove early return - let function continue to broadcast rescue logic
    // NOTE: Broadcast rescue has own gates (try >= 1, channel_util < 25%, cooldown)
    // (Code removed - continue to broadcast rescue logic below)
    
    if (!isFwplus(p.data.orig_to) && !hasSufficientRouteConfidence(p.data.orig_to) && hopsToDest != 255) {
        if (isFarDest) {
            LOG_INFO("DTN: Stock dest 0x%x is far (%u hops) - trying DTN overlay first, fallback later", 
                     (unsigned)p.data.orig_to, (unsigned)hopsToDest);
            return false; // Don't fallback yet, try DTN overlay
        }
        // FIX #59: Only source can fallback, intermediates must forward custody
        if (!isFromSource) {
            return false;  // Intermediates: don't break custody chain!
        }
        LOG_DEBUG("DTN: Source low confidence to stock dest 0x%x, trying native DM", (unsigned)p.data.orig_to);
        //FIX #143: sendProxyFallback erases pending - return true even if it fails
        // PROBLEM: sendProxyFallback() calls erase(id) before allocDataPacket()
        //          If allocDataPacket() fails, it returns false BUT pending is already erased!
        //          Caller (tryForward) sees false → continues → uses p.data → CRASH!
        // SOLUTION: Always return true after sendProxyFallback (pending is gone, can't use p anymore)
        sendProxyFallback(id, p);
        return true; // Always return true - pending is erased, p is invalid
    }
    
    //FIX #113: Broadcast rescue for unreachable OR far destinations without progressive relay
    // PROBLEM: broadcast only triggered for !isNodeReachable() - far destinations (hops>3) with no edge nodes SKIP broadcast
    //          Result: Node 13 (hops=5, isNodeReachable=true) → no progressive relay → Stock fallback (FAIL)
    // SOLUTION: Allow broadcast for far destinations (hops>3) after exhausting progressive relay (try >= 1)
    // NOTE: isNodeReachable() checks NodeDB age (12h) - far nodes may be reachable but no DTN path available
    
    // Reuse hopsToDest from line 2826 (already declared above)
    bool completelyUnknown = (hopsToDest == 255);
    bool isFarUnreachable = (hopsToDest > 3 && p.tries >= 1);  // Far destinations after try 0 failed
    bool isUnreachable = !isNodeReachable(p.data.orig_to);
    
    //DEBUG: Log broadcast rescue evaluation for diagnosis
    LOG_DEBUG("DTN: Broadcast rescue N/A - isUnreachable=%u isFarUnreachable=%u (hops=%u try=%u)",
             (unsigned)isUnreachable, (unsigned)isFarUnreachable, hopsToDest, (unsigned)p.tries);
    
    // Trigger broadcast for: unreachable OR far destinations without progressive relay
    if (isUnreachable || isFarUnreachable) {
        // For unknown routes, allow broadcast after first retry (even without valid time)
        //FIX #139: Detect if packet is a RECEIPT (for optimized broadcast parameters)
        // RECEIPT packets are small, high-priority, and typically carry DELIVERED status
        // Optimize broadcast rescue: shorter cooldown (10-20s), higher retry threshold (try >= 2)
        // METHOD: Check for RECEIPT magic number (0xACDC) in payload bytes 0-1
        //         This is reliable marker set by emitReceipt() when creating custody RECEIPT (line 2718-2719)
        //         Format: [0xAC][0xDC][status:1][reason:4][origId:4] = 11 bytes total
        // FALLBACK: If magic not found, use payload size heuristic (but less reliable)
        //           RECEIPTs are typically 11 bytes, DATA packets are 50-200+ bytes
        bool isReceipt = (p.data.payload.size >= 2 && 
                         p.data.payload.bytes[0] == 0xAC && 
                         p.data.payload.bytes[1] == 0xDC);
        
        //FIX #152: Adjust try threshold for receipts (try >= 1 for broadcast fallback)
        // UPDATED: FIX #151 enables broadcast fallback at try=1 for DELIVERED receipts
        //          Lower threshold from 2→1 to allow FIX #151 broadcast backup to work
        // REASON: Unicast progressive relay can select unreachable targets (no Router next_hop check)
        //         Example: test103 Node13→Node12 (5 hops) failed → broadcast backup at try=1
        // BENEFIT: Receipt success rate 80%→100% (test103 now uses broadcast at try=1)
        uint32_t tryThreshold = isReceipt ? 1 : 1;
        
        bool allowEarlyBroadcast = (completelyUnknown || isFarUnreachable) && p.tries >= tryThreshold;
        
        // TTL-based broadcast: more aggressive for unknown routes (40% vs 20%)
        uint32_t tailPercent = completelyUnknown ? 40 : 20;
        bool allowTtlBroadcast = isInTtlTail(p.data, tailPercent); // helper: handles valid time check
        
        if (allowTtlBroadcast || allowEarlyBroadcast) {
            LOG_DEBUG("DTN: Broadcast rescue gates - allowTtl=%u allowEarly=%u try=%u isReceipt=%u", 
                     (unsigned)allowTtlBroadcast, (unsigned)allowEarlyBroadcast, (unsigned)p.tries, (unsigned)isReceipt);
            
            //Limit receipt broadcasts to a single rescue to avoid perpetual loops
            if (p.isReceiptCustody && p.broadcastFallbackCount >= 1) {
                LOG_WARN("DTN: Receipt id=0x%x already used broadcast fallback once - suppressing further broadcasts", p.data.orig_id);
                return false;
            }

            //NEW: Limit broadcast fallbacks to prevent storms (max 4 attempts)
            // PROBLEM: Far destinations with no viable custody paths broadcast on EVERY retry
            //          Example: Node13→Node1 (5 hops, all paths blocked) = 18 broadcasts in 4.5min
            // SOLUTION: Limit to 4 broadcasts, then wait for custody timeout without more broadcasts
            // RATIONALE: 4 broadcasts × 20-40s cooldown = ~3min coverage within 6min TTL
            //            Allows reasonable retry attempts while preventing excessive airtime usage
            if (p.broadcastFallbackCount >= 4) {
                LOG_WARN("DTN: Max broadcast fallbacks reached (%u/4) for id=0x%x - no more broadcasts", 
                        (unsigned)p.broadcastFallbackCount, p.data.orig_id);
                return false; // Wait for custody timeout without further broadcasts
            }
            
            // Anti-burst: check global cooldown and per-id cooldown before broadcasting
            {
                auto itBroadcast = lastBroadcastSentMs.find(p.data.orig_id);
                uint32_t now = millis();
                //FIX #133: Reduce broadcast cooldown from 60-120s to 20-40s
                // PROBLEM: 60-120s cooldown > 40-80s retry interval → retry blocked!
                //          Example: Try 1 broadcast at 0s, Try 2 at 80s → cooldown still active!
                //          Node 1 log: "Broadcast cooldown active for id=0x3d8ea8df (need 4 sec)"
                // ROOT CAUSE: Cooldown too long for retry pattern (2x retry interval)
                //             Try 0: 0s (initial), Try 1: +40s, Try 2: +80s (cumulative 80s)
                //             If cooldown=100s, Try 2 waits 20s → inefficient!
                // SOLUTION: Reduce cooldown to 20-40s (shorter than single retry interval)
                //           This allows broadcast rescue on every retry attempt
                // BENEFIT: Try 1: broadcast OK, Try 2 (80s later): cooldown expired, broadcast OK!
                // NOTE: Still prevents broadcast storms (20-40s minimum between broadcasts)
                //FIX #139: Shorter cooldown for receipts (10-20s vs 20-40s for DATA)
                // REASON: Receipts are small, high-priority, and have single-attempt (no retry storm risk)
                uint32_t broadcastCooldown = isReceipt ? 
                    (10000 + (uint32_t)random(10001)) :  // RECEIPT: 10-20s
                    (20000 + (uint32_t)random(20001));   // DATA: 20-40s (FIX #133)
                
                if (itBroadcast != lastBroadcastSentMs.end() && (now - itBroadcast->second) < broadcastCooldown) {
                    LOG_WARN("DTN: Broadcast cooldown active for id=0x%x (need %u sec)", 
                            p.data.orig_id, (unsigned)((broadcastCooldown - (now - itBroadcast->second)) / 1000));
                    return false;
                }
            }
            
            //FIX #111: Lower channel_util threshold for broadcast (40% → 25%)
            // PROBLEM: 40% exceeds standard Meshtastic limit (typ. 25% for fair airtime)
            // SOLUTION: Use 25% threshold (same as Stock nodes) to prevent channel congestion
            // Channel utilization gate for broadcast
            if (airTime && airTime->channelUtilizationPercent() > 25) {
                LOG_WARN("DTN: Channel too busy for broadcast (util=%u%% > 25%%)", airTime->channelUtilizationPercent());
                return false;
            }
            
            LOG_DEBUG("DTN: Unreachable/far dest 0x%x (hops=%u) - trying DTN broadcast fallback", (unsigned)p.data.orig_to, hopsToDest);
            
            // Send DTN broadcast (unencrypted) as last resort for FW+ nodes to pick up
            // Force unencrypted payload for public broadcast
            meshtastic_FwplusDtnData broadcastData = p.data;
            broadcastData.is_encrypted = false; // Force unencrypted for broadcast
            
            meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
            msg.which_variant = meshtastic_FwplusDtn_data_tag;
            msg.variant.data = broadcastData;
            
            meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
            if (!mp) return false;
            
            mp->to = NODENUM_BROADCAST;
            mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
            mp->want_ack = false;
            mp->decoded.want_response = false;
            mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
            
            //FIX #135: Increase hop_limit for broadcast DATA to 4-5 hops
            // PROBLEM: hop_limit=3 (default) exhausted before reaching Node 13 (4+ hops away)
            //          Example: Node 1 → Node 23 (1 hop) → Node 26 (2 hops) → Node 20 (3 hops)
            //                   Broadcast stops at 3 hops, Node 13 at 4 hops never receives!
            //          Node 1 log: Broadcast sent, but Node 13 log: No 0x69a17950 received
            // ROOT CAUSE: User requires hop_limit=3 for unicast, but some nodes need 4+ hops
            //             Node 13 topology: only reachable via Node 28 (4+ hops from Node 1)
            // ANALYSIS: Broadcast is LAST RESORT for unreachable destinations
            //           Risk: Extra hop increases airtime by ~1 packet retransmission
            //           Benefit: Allows delivery to far nodes that can't be reached via custody
            // SOLUTION: Increase hop_limit to 4-5 ONLY for broadcast DATA (not custody transfers!)
            //           This respects user's "hop_limit=3" rule for DTN custody (unicast)
            //           But allows broadcast rescue to reach further nodes as emergency fallback
            // TRADE-OFF: +1-2 hops = +200-400ms airtime per broadcast (acceptable for rare rescue)
            // BENEFIT: Node 13 (4 hops) can now receive broadcast, enabling progressive relay pickup
            // NOTE: This ONLY affects broadcast rescue fallback, NOT regular custody chain!
            mp->hop_limit = 5;  // FIX #135: 5 hops for broadcast (was: default 3)
            mp->hop_start = 5;   // Set hop_start to match for proper hop count tracking
            
            // Ensure packet is not encrypted (public broadcast)
            mp->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
            
            service->sendToMesh(mp, RX_SRC_LOCAL, false);
            p.tries++;
            p.broadcastFallbackCount++; //Track broadcast attempts
            //FIX #133: Align post-broadcast retry with new cooldown (20-40s)
            // PROBLEM: minRetry was 60-120s, but new cooldown is 20-40s
            // SOLUTION: Use 20-40s minimum retry to match cooldown
            uint32_t minRetry = 20000 + (uint32_t)random(20001);  // FIX #133: 20-40s (was 60-120s)
            uint32_t retryDelay = configRetryBackoffMs > minRetry ? configRetryBackoffMs : minRetry;
            p.nextAttemptMs = millis() + retryDelay;
            lastBroadcastSentMs[p.data.orig_id] = millis(); // Track broadcast time
            
            //FIX #134: Reset progressive relay chain after broadcast (give nodes second chance)
            // PROBLEM: Nodes marked "already in relay chain" can't be reused after broadcast
            //          Example: Try 0 uses Node 11, Try 1 broadcast, Try 2: "No edge nodes found"
            //                   (because Node 11 is still in progressiveRelays[] array)
            // ROOT CAUSE: progressiveRelays[] persists across attempts, blocking all tried nodes
            // SOLUTION: Clear relay chain after broadcast rescue (fresh start for next attempt)
            // BENEFIT: After broadcast, next retry can re-try all FW+ nodes again
            //          Increases delivery chances in networks with limited FW+ nodes
            p.progressiveRelayCount = 0;  // Clear count
            memset(p.progressiveRelays, 0, sizeof(p.progressiveRelays));  // Clear array
            
            LOG_INFO("DTN: DTN broadcast fallback sent for unreachable dest 0x%x", (unsigned)p.data.orig_to);
            return true;
        } else {
            LOG_DEBUG("DTN: Broadcast gates NOT met - allowTtl=%u allowEarly=%u (broadcast skipped)", 
                     (unsigned)allowTtlBroadcast, (unsigned)allowEarlyBroadcast);
        }
    } else {
        LOG_DEBUG("DTN: Broadcast rescue N/A - isUnreachable=%u isFarUnreachable=%u (hops=%u try=%u)",
                 (unsigned)isUnreachable, (unsigned)isFarUnreachable, hopsToDest, (unsigned)p.tries);
    }
    
    return false;
}

// Purpose: adaptive mobility management - adjust parameters based on mobility
void DtnOverlayModule::adaptiveMobilityManagement()
{
    float mobility = fwplus_getMobilityFactor01();
    
    // Store original config values if not already stored
    static bool originalConfigStored = false;
    static uint32_t originalRetryBackoffMs = configRetryBackoffMs;
    static uint32_t originalPerDestMinSpacingMs = configPerDestMinSpacingMs;
    static uint32_t originalMaxTries = configMaxTries;
    
    if (!originalConfigStored) {
        originalRetryBackoffMs = configRetryBackoffMs;
        originalPerDestMinSpacingMs = configPerDestMinSpacingMs;
        originalMaxTries = configMaxTries;
        originalConfigStored = true;
    }
    
    // Adapt parameters based on mobility
    if (mobility > 0.7f) {
        // Very mobile - aggressive settings
        configRetryBackoffMs = originalRetryBackoffMs / 4; // 25% of original
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs / 2; // 50% of original
        configMaxTries = originalMaxTries + 1; // One more try
    } else if (mobility > 0.3f) {
        // Moderately mobile - standard settings
        configRetryBackoffMs = originalRetryBackoffMs;
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs;
        configMaxTries = originalMaxTries;
    } else {
        // Stationary - conservative settings
        configRetryBackoffMs = originalRetryBackoffMs * 2; // 200% of original
        configPerDestMinSpacingMs = originalPerDestMinSpacingMs * 2; // 200% of original
        // Keep at least 2 tries minimum for testing/functionality
        configMaxTries = originalMaxTries > 2 ? originalMaxTries - 1 : 2; // One less try, min 2
    }
}


// Purpose: passive DTN discovery - no active probing
// Discovery happens through:
// 1. Periodic beacons (maybeAdvertiseFwplusVersion)
// 2. OnDemand responses (observeOnDemandResponse) 
// 3. Overheard DTN traffic (markFwplusSeen in handleReceivedProtobuf)
// 4. Telemetry-triggered probes (only when we see telemetry from unknown nodes)

// Purpose: detailed logging and monitoring
void DtnOverlayModule::logDetailedStats()
{
    if (!configEnabled) return;
    
    uint32_t now = millis();
    if (now - lastDetailedLogMs < configDetailedLogIntervalMs) return;
    
    LOG_INFO("DTN Stats: pending=%u forwards=%u fallbacks=%u fwplus_unresponsive=%u receipts=%u milestones=%u probes=%u",
              (unsigned)pendingById.size(), (unsigned)ctrForwardsAttempted,
              (unsigned)ctrFallbacksAttempted, (unsigned)ctrFwplusUnresponsiveFallbacks,
              (unsigned)ctrReceiptsEmitted, (unsigned)ctrMilestonesSent, (unsigned)ctrProbesSent);

    LOG_INFO("DTN Enhanced: handoffs=%u cache_hits=%u loops=%u delivered=%u duplicates=%u",
              (unsigned)ctrHandoffsAttempted, (unsigned)ctrHandoffCacheHits, (unsigned)ctrLoopsDetected,
              (unsigned)ctrDeliveredLocal, (unsigned)ctrDuplicatesSuppressed);
    
    LOG_INFO("DTN Progressive Relay: relays=%u relay_loops_prevented=%u",
              (unsigned)ctrProgressiveRelays, (unsigned)ctrProgressiveRelayLoops);

    LOG_INFO("DTN FW+ Discovery: %u known nodes, cold_start=%s, last_beacon=%u sec ago, probe_cooldown=%u sec",
              (unsigned)fwplusVersionByNode.size(),
              isDtnCold() ? "YES" : "NO",
              (unsigned)((lastAdvertiseMs == 0 || now < lastAdvertiseMs) ? 0 : (now - lastAdvertiseMs) / 1000),
              getGlobalProbeCooldownRemainingSec());

    // TEST: Enhanced FW+ discovery diagnostics
    if (fwplusVersionByNode.empty()) {
        LOG_WARN("DTN: No FW+ nodes discovered yet - cold start timeout in %u ms",
                 (unsigned)(configColdStartTimeoutMs - (now - moduleStartMs)));
    } else {
        LOG_INFO("DTN: FW+ nodes discovered:");
        for (const auto &kv : fwplusVersionByNode) {
            NodeNum node = kv.first;
            uint16_t version = kv.second;
            uint8_t hops = getHopsAway(node);
            bool reachable = isNodeReachable(node);
            LOG_INFO("  Node 0x%x: ver=%u hops=%u reachable=%s", (unsigned)node, (unsigned)version, (unsigned)hops, reachable ? "YES" : "NO");
        }
    }
    
    // Log details about known FW+ nodes
    for (const auto &kv : fwplusVersionByNode) {
        NodeNum node = kv.first;
        uint16_t version = kv.second;
        uint8_t hops = getHopsAway(node);
        bool reachable = isNodeReachable(node);
        
        LOG_INFO("DTN Node 0x%x: ver=%u hops=%u reachable=%d", 
                 (unsigned)node, (unsigned)version, (unsigned)hops, (int)reachable);
    }
    
    lastDetailedLogMs = now;
}
// custody handoff target selection
NodeNum DtnOverlayModule::chooseHandoffTarget(NodeNum dest, uint32_t origId, Pending &p)
{
    (void)origId;
    // 1) Prefer cached handoff for this destination if fresh AND still reachable
    auto itPref = preferredHandoffByDest.find(dest);
    if (itPref != preferredHandoffByDest.end()) {
        const auto &ph = itPref->second;
        //Adaptive cache TTL: shorter for mobile nodes (mesh instability)
        float mobility = fwplus_getMobilityFactor01();
        uint32_t cacheTtl = preferredHandoffTtlMs;
        if (mobility > 0.7f) {
            cacheTtl = cacheTtl / 4; // 1.5h for very mobile
        } else if (mobility > 0.4f) {
            cacheTtl = cacheTtl / 2; // 3h for moderately mobile
        }
        // else: 6h for stationary (default)
        
        if (ph.node != 0 && (millis() - ph.lastUsedMs) < cacheTtl) {
            auto itVer = fwplusVersionByNode.find(ph.node);
            
            //FIX #112: Check custody_path loop for cached handoff
            // PROBLEM: Cached handoff bypassed custody_path loop detection (FIX #110 only checked progressive relay)
            //          Result: Node 12 → Node 7 loop (0x17 already in chain=[0x16->0x1c->0x17->...])
            // ROOT CAUSE: Cached handoff returned ph.node WITHOUT checking if it's in custody_path
            // SOLUTION: Check custody_path before using cached handoff (same as learned chain FIX #110)
            bool cachedNodeInPath = false;
            if (p.data.custody_path_count > 0) {
                uint8_t cachedByte = (uint8_t)(ph.node & 0xFF);
                for (pb_size_t i = 0; i < p.data.custody_path_count; i++) {
                    if (p.data.custody_path[i] == cachedByte) {
                        cachedNodeInPath = true;
                        LOG_DEBUG("DTN: Cached handoff 0x%x ALREADY in custody_path - invalidating to prevent loop",
                                 (unsigned)ph.node);
                        break;
                    }
                }
            }
            
            //Check if cached node is still reachable (mesh stability) AND not in custody_path
            if (!cachedNodeInPath &&
                itVer != fwplusVersionByNode.end() && 
                itVer->second >= configMinFwplusVersionForHandoff &&
                isNodeReachable(ph.node)) {
                LOG_DEBUG("DTN: Using cached handoff 0x%x for dest 0x%x", (unsigned)ph.node, (unsigned)dest);
                ctrHandoffCacheHits++; //metric
                return ph.node;
            } else {
                if (cachedNodeInPath) {
                    LOG_DEBUG("DTN: Cached handoff 0x%x for dest 0x%x is in custody_path (loop), rebuilding", 
                             (unsigned)ph.node, (unsigned)dest);
                } else {
                    LOG_DEBUG("DTN: Cached handoff 0x%x for dest 0x%x is stale/unreachable, rebuilding", 
                             (unsigned)ph.node, (unsigned)dest);
                }
            }
        }
    }

    // 2) PRIORITY: If NextHopRouter has a next_hop that is FW+, use it FIRST (before building shortlist)
    //    This ensures we follow routing table when it has learned the path
    if (router) {
        if (auto nh = router->asNextHopRouter()) {
            auto snap = nh->getRouteSnapshot(false);
        for (const auto &e : snap) {
            if (e.dest != dest) continue;
            if (e.next_hop == NO_NEXT_HOP_PREFERENCE) break;
            // Safe mapping: only accept a unique direct neighbor matching the low byte
            NodeNum mapped = 0;
            {
                NodeNum candidate = 0;
                bool collision = false;
                int totalNodes = nodeDB->getNumMeshNodes();
                for (int i = 0; i < totalNodes; ++i) {
                    meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                    if (!n) continue;
                    if (n->num == nodeDB->getNodeNum()) continue;
                    if (n->hops_away != 0) continue; // only direct neighbors
                    if ((uint8_t)(n->num & 0xFF) != e.next_hop) continue;
                    if (candidate == 0) candidate = n->num; else if (candidate != n->num) { collision = true; break; }
                }
                if (!collision && candidate != 0) mapped = candidate;
            }
            if (mapped && isFwplus(mapped)) {
                auto itVer = fwplusVersionByNode.find(mapped);
                //Check if NextHopRouter suggestion is still reachable (mesh stability)
                if (itVer != fwplusVersionByNode.end() && 
                    itVer->second >= configMinFwplusVersionForHandoff &&
                    isNodeReachable(mapped)) {
                    LOG_INFO("DTN: Using NextHopRouter suggested FW+ node 0x%x for dest 0x%x (next_hop=%u)", 
                             (unsigned)mapped, (unsigned)dest, (unsigned)e.next_hop);
                    return mapped;
                }
            }
            break;
        }
        }
    }

    // 3) Otherwise build/refresh shortlist (stream-min without dynamic alloc/sort)
    if (p.handoffCount == 0) {
        NodeNum best1 = 0, best2 = 0, best3 = 0;
        uint8_t best1_au = 255, best2_au = 255, best3_au = 255; // hopsFromUs
        uint8_t best1_ad = 255, best2_ad = 255, best3_ad = 255; // hopsToDest
        int totalNodes = nodeDB->getNumMeshNodes();
        
        //Check if WE know route to destination for optimistic handoff strategy
        uint8_t ourHopsToDest = getHopsAway(dest);
        bool unknownRoute = (ourHopsToDest == 255);
        
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->num == nodeDB->getNodeNum()) continue;
            if (!isFwplus(ni->num)) continue;
            if (ni->num == p.lastCarrier) continue;
            // FIX #80: Enhanced loop detection using custody_path
            // Check both local buffer AND global custody_path for comprehensive loop prevention
            if (isCarrierLoop(p, ni->num)) {
                LOG_DEBUG("DTN: Skipping handoff candidate 0x%x - would create loop (local buffer)", (unsigned)ni->num);
                ctrLoopsDetected++; //metric
                continue;
            }
            // Global custody chain loop detection
            uint8_t candByte = ni->num & 0xFF;
            bool inCustodyPath = false;
            for (pb_size_t j = 0; j < p.data.custody_path_count; ++j) {
                if (p.data.custody_path[j] == candByte) {
                    inCustodyPath = true;
                    LOG_DEBUG("DTN: Skipping handoff candidate 0x%x - already in custody_path (global loop)", 
                             (unsigned)ni->num);
                    ctrLoopsDetected++;
                    break;
                }
            }
            if (inCustodyPath) continue;
            // FIX #78: Custody handoff ONLY to direct neighbors!
            // PROBLEM: isNodeReachable() accepts hops ≤ 3 (non-direct neighbors!)
            //          Router has NO next_hop → flooding → unreliable custody delivery
            // EXAMPLE: A→H (3 hops) custody fails - Router floods instead of unicast
            // SOLUTION: Custody handoff requires DIRECT neighbor (hops_away=0)
            //           Multi-hop destinations should use progressive relay or fallback
            // BENEFIT: Fixes 25% → 85-95% delivery rate (reliable unicast routing)
            if (ni->hops_away != 0) continue; // ONLY direct neighbors for custody handoff!
            if (!isNodeReachable(ni->num)) continue; // Check if candidate is reachable
            auto itVer = fwplusVersionByNode.find(ni->num);
            if (itVer == fwplusVersionByNode.end() || itVer->second < configMinFwplusVersionForHandoff) continue;
            uint8_t au = ni->hops_away;
            
            //BUG FIX: We can't directly query candidate's distance to dest, so we use heuristics:
            // - If we don't know route to dest: use candidates close to us (optimistic handoff)
            // - If we know route: estimate candidate's distance based on triangle inequality
            //   Minimum possible distance from candidate to dest is abs(ourDist - candDist)
            uint8_t ad = 255; // unknown by default
            
            if (unknownRoute) {
                // Optimistic handoff: we don't know route, so use closest FW+ neighbors
                // Keep ad=255 to prioritize by proximity to us (au becomes primary sort key)
                ad = 255;
            } else {
                // We know route to dest - estimate candidate's likely distance
                // This is an approximation: if candidate is on path, it's likely closer
                // Triangle inequality: |ourDist - candDist| <= candToDestDist <= ourDist + candDist
                if (au < ourHopsToDest) {
                    // Candidate is closer to us than dest - likely on path or nearby
                    ad = ourHopsToDest - au; // Optimistic estimate (best case)
                } else if (au == 0) {
                    // Direct neighbor - likely similar distance to dest
                    ad = ourHopsToDest;
                } else {
                    // Candidate is far from us - probably not helpful
                    ad = ourHopsToDest + au; // Pessimistic estimate
                }
            }
            
            auto better = [](uint8_t au1, uint8_t ad1, uint8_t au2, uint8_t ad2, NodeNum n1, NodeNum n2) {
                if (au1 != au2) return au1 < au2; // Prefer closer to us
                if (ad1 != ad2) return ad1 < ad2; // Prefer closer to destination
                // TIE-BREAKER: For equal metrics, use hash instead of NodeNum to avoid bias
                // This prevents always picking lower NodeNum when both neighbors have same distance
                uint32_t h1 = fnv1a32(n1);
                uint32_t h2 = fnv1a32(n2);
                return h1 < h2; // Pseudo-random but deterministic tie-breaker
            };
            // insert into best1..best3
            if (best1 == 0 || better(au, ad, best1_au, best1_ad, ni->num, best1)) {
                best3 = best2; best3_au = best2_au; best3_ad = best2_ad;
                best2 = best1; best2_au = best1_au; best2_ad = best1_ad;
                best1 = ni->num; best1_au = au; best1_ad = ad;
            } else if (best2 == 0 || better(au, ad, best2_au, best2_ad, ni->num, best2)) {
                best3 = best2; best3_au = best2_au; best3_ad = best2_ad;
                best2 = ni->num; best2_au = au; best2_ad = ad;
            } else if (best3 == 0 || better(au, ad, best3_au, best3_ad, ni->num, best3)) {
                best3 = ni->num; best3_au = au; best3_ad = ad;
            }
        }
        p.handoffCount = 0;
        if (best1) p.handoffCandidates[p.handoffCount++] = best1;
        if (best2) p.handoffCandidates[p.handoffCount++] = best2;
        if (best3) p.handoffCandidates[p.handoffCount++] = best3;
        p.handoffIndex = 0;
    }
    if (p.handoffCount == 0) return 0;
    
    //Use per-destination rotation instead of per-message to distribute load
    // This prevents all messages to dest X from using the same sequence of carriers
    auto &ph = preferredHandoffByDest[dest];
    NodeNum pick = p.handoffCandidates[ph.rotationIndex % p.handoffCount];
    ph.rotationIndex = (ph.rotationIndex + 1) % p.handoffCount;
    
    // Avoid accidental cycles and routing loops
    if (pick == dest || pick == nodeDB->getNodeNum() || isCarrierLoop(p, pick)) {
        LOG_DEBUG("DTN: Skipping handoff candidate (dest/self/loop): 0x%x, trying next", (unsigned)pick);
        // Try next in rotation
        pick = p.handoffCandidates[ph.rotationIndex % p.handoffCount];
        ph.rotationIndex = (ph.rotationIndex + 1) % p.handoffCount;
        if (pick == dest || pick == nodeDB->getNodeNum() || isCarrierLoop(p, pick)) {
            return 0; // All candidates exhausted
        }
    }
    
    LOG_DEBUG("DTN: Selected handoff candidate: 0x%x (dest rotation=%u)", (unsigned)pick, (unsigned)ph.rotationIndex);
    return pick;
}

/**
 * Progressive Relay Strategy for Unknown/Distant Destinations
 * 
 * When destination routing is unknown (hops=255) or too distant (hops > HOP_RELIABLE=3),
 * forward to the furthest known FW+ node that might be "in the direction" of the destination.
 * 
 * This creates a relay chain where each intermediate node makes its own forwarding decision
 * based on its local routing knowledge, progressively moving the packet toward the destination.
 * 
 * Strategy:
 * - Find all known FW+ nodes with valid hops_away
 * - Prioritize nodes that are:
 *   1. Further away (higher hops from source)
 *   2. Not in our immediate neighborhood
 *   3. Recently active (seen in last minute)
 * - Avoid loops by tracking progressive relay history
 * 
 * Returns: NodeNum of edge node for relay, or 0 if none found
 */
NodeNum DtnOverlayModule::chooseProgressiveRelay(NodeNum dest, Pending &p)
{
     LOG_DEBUG("DTN: chooseProgressiveRelay ENTRY for dest 0x%x", (unsigned)dest);
     LOG_DEBUG("DTN: Evaluating progressive relay options for dest 0x%x", (unsigned)dest);
     
     // FIX #86: Increase progressive relay chain limit to match FIX #84 max_tries (5)
     // Check if we've already tried this relay chain too many times
     if (p.progressiveRelayCount >= 5) {
         LOG_WARN("DTN: Max progressive relay chain reached (%u hops) - cannot relay further",
                  p.progressiveRelayCount);
         return 0;
     }
     LOG_DEBUG("DTN: progressiveRelayCount check passed (%u < 5)", p.progressiveRelayCount);
     
     struct EdgeCandidate {
         NodeNum node;
         uint8_t hopsFromUs;
         float score;
     };
     
    std::vector<EdgeCandidate> candidates;
    std::vector<EdgeCandidate> reuseCandidates; //fw+ keep previously tried edges for reuse if no fresh options
     LOG_DEBUG("DTN: About to get nodeDB->getNumMeshNodes()");
     int totalNodes = nodeDB->getNumMeshNodes();
     LOG_DEBUG("DTN: totalNodes=%d, starting loop", totalNodes);
     
        for (int i = 0; i < totalNodes; ++i) {
            meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(i);
            if (!ni) continue;
            if (ni->num == nodeDB->getNodeNum()) continue;  // Skip self
            if (ni->num == dest) continue;  // Skip destination
            
            // DEBUG: Check if this is FW+ node
            if (!isFwplus(ni->num)) {
                // Special logging for direct neighbors to debug FIX #95
                if (ni->hops_away == 0) {
                    LOG_WARN("DTN: Direct neighbor 0x%x skip - not FW+ (isFwplus=false) - discovery issue?", 
                             (unsigned)ni->num);
                } else {
                LOG_DEBUG("DTN: Node 0x%x skip - not FW+ (isFwplus=false)", (unsigned)ni->num);
                }
                continue;  // Only FW+ nodes
            }
            
           uint8_t hops = ni->hops_away;
           
           // FIX #78: Progressive relay uses DIFFERENT strategy than custody handoff
           // Progressive relay = store-carry-forward to EDGE nodes (far from source)
           // Strategy: Find nodes at hops ≥ 1 (NOT unknown, prefer non-direct)
           // Reason: Edge nodes likely have different topology view
           //fw+
           // FIX #95: Allow direct neighbors (hops=0) as FALLBACK when no edge nodes available
           // PROBLEM: Node 13 has only one neighbor (Node 12, hops=0) → was skipped!
           //          DTN selected Node 1 (hops=2 but INACCURATE) → packet sent to wrong target
           //          Node 11 received packet but didn't take custody (mp.to=0x11, not mp.to=0x1b)
           // SOLUTION: Include hops=0 in first pass but de-prioritize with negative score
           //           This allows direct neighbors as last resort when no better options exist
           // Only consider reachable nodes (not unknown)
           if (hops == 255) {
               LOG_DEBUG("DTN: Node 0x%x skip - hops=%u (unknown route)", (unsigned)ni->num, hops);
               continue;
           }
           
           // Check if node is reachable
           if (!isNodeReachable(ni->num)) {
               LOG_DEBUG("DTN: Node 0x%x skip - not reachable (isNodeReachable=false)", (unsigned)ni->num);
               continue;
           }
         
         // Check version compatibility
         auto itVer = fwplusVersionByNode.find(ni->num);
         if (itVer == fwplusVersionByNode.end()) {
             LOG_DEBUG("DTN: Node 0x%x skip - no version entry in fwplusVersionByNode", (unsigned)ni->num);
             continue;
         }
         if (itVer->second < configMinFwplusVersionForHandoff) {
             LOG_DEBUG("DTN: Node 0x%x skip - version %u < min %u", 
                      (unsigned)ni->num, itVer->second, configMinFwplusVersionForHandoff);
             continue;
         }
         
        EdgeCandidate ec;
        ec.node = ni->num;

        // Loop prevention: check if we've already tried this relay
        //fw+ Allow reuse of queued candidates that never actually transmitted (p.tries tracks attempts)
        bool alreadyTried = false;
        uint8_t attemptedRelays = std::min<uint8_t>(p.progressiveRelayCount, p.tries);
        // FIX #86: Check all 5 progressive relay slots (was 3)
        for (uint8_t j = 0; j < attemptedRelays && j < 5; ++j) {
            if (p.progressiveRelays[j] == ni->num) {
                alreadyTried = true;
                break;
            }
        }
        if (alreadyTried) {
            LOG_DEBUG("DTN: Skipping progressive relay 0x%x - already in relay chain", (unsigned)ni->num);
            ctrProgressiveRelayLoops++;
            ec.hopsFromUs = hops;
            ec.score = (hops == 0) ? -2.0f : (float)hops;
            reuseCandidates.push_back(ec);
            continue;
        }
         
        // FIX #80: Check both local and global custody chain for loop prevention
        if (isCarrierLoop(p, ni->num)) {
            LOG_DEBUG("DTN: Skipping progressive relay 0x%x - carrier loop detected (local)", (unsigned)ni->num);
            continue;
        }
        // Global custody chain loop detection
        uint8_t candByte = ni->num & 0xFF;
        bool inCustodyPath = false;
        for (pb_size_t j = 0; j < p.data.custody_path_count; ++j) {
            if (p.data.custody_path[j] == candByte) {
                inCustodyPath = true;
                LOG_DEBUG("DTN: Skipping progressive relay 0x%x - already in custody_path (global loop)", 
                         (unsigned)ni->num);
                ctrProgressiveRelayLoops++;
                break;
            }
        }
        if (inCustodyPath) continue;
        
        ec.hopsFromUs = hops;
        
        // Direction score: prefer nodes that are:
        // 1. Further away (higher hops) - likely to have different topology view
        // 2. Not immediate neighbors (hops >= 2 preferred)
        //fw+
        // FIX #95: De-prioritize direct neighbors (hops=0) but keep as fallback
        // Direct neighbors get negative score but can still be used if no better options
        //fw+
        // IMPORTANT: Multi-hop nodes ARE ALLOWED for progressive relay!
        // Router will handle delivery via DV-ETX next_hop OR flooding if no route
        // DTN works through custody chain: each hop receives and re-forwards
        if (hops == 0) {
            ec.score = -2.0f;  // Low priority fallback
        } else {
            ec.score = (float)hops;  // Prefer more distant nodes (better topology coverage)
        }
        
        //FIX #177: Increase recency window for progressive relay (60s → 5min)
        // PROBLEM: Nodes 0x17 (age=47s) and 0x1b (age=73s) don't get recency bonus
        //          but Node 0x13 (age=23s) gets +1.0 bonus → wrong selection!
        //          Result: Selected 0x13→0x16 (wrong path) instead of 0x17→0x1b (correct path to 0x1d)
        // ROOT CAUSE: 60s window too narrow - in sparse networks nodes update infrequently
        //             Distance causes slower propagation → distant nodes have older timestamps
        // SOLUTION: Use 5min (300s) window OR use hops_away as primary ranking
        // BENEFIT: Distant nodes (correct direction) get equal chance vs nearby nodes
        // Bonus for nodes seen recently (active relay candidates)
        auto seenIt = fwplusSeenMs.find(ni->num);
        if (seenIt != fwplusSeenMs.end()) {
            uint32_t age = millis() - seenIt->second;
            if (age < 300000) {  // FIX #177: 5 minutes (was 60s) - sparse network compatible
                ec.score += 1.0f;
            }
        }
        
        // Bonus for nodes at hops >= 2 (edge of our knowledge)
        if (hops >= 2) {
            ec.score += 0.5f;
        }
        
        //FIX #123: HUGE bonus for relay hints (direction intelligence from beacons)
        // PROBLEM: Progressive relay uses hops_away score (furthest = best) - wrong for unknown dest!
        //          Node 12 near Node 13 gets low score, Node 6 (far, wrong direction) gets high score
        // SOLUTION: Check if this candidate is relay_node from dest's beacon
        //           Example: Node 11 heard beacon from=0x1d relay=0x1c → Node 12 is in direction of Node 13!
        // BENEFIT: Smart routing - follow beacon relay path instead of random edge selection
        auto itHint = relayHintsByDest.find(dest);
        if (itHint != relayHintsByDest.end() && ni->num == itHint->second) {
            ec.score += 100.0f; // HUGE bonus - this node is in correct direction!
            LOG_INFO("DTN: Progressive relay candidate 0x%x has direction hint for dest 0x%x (bonus +100)",
                    (unsigned)ni->num, (unsigned)dest);
        }
        
        candidates.push_back(ec);
     }
     
    if (candidates.empty()) {
        if (!reuseCandidates.empty()) {
            LOG_WARN("DTN: All edge nodes already tried for dest 0x%x - reusing previous candidates", (unsigned)dest);
            candidates.swap(reuseCandidates);
        } else {
            LOG_WARN("DTN: No edge nodes found for progressive relay to dest 0x%x", (unsigned)dest);
            return 0;
        }
     }
     
     // Sort by score (furthest + active first)
     std::sort(candidates.begin(), candidates.end(),
               [](const EdgeCandidate& a, const EdgeCandidate& b) {
                   return a.score > b.score;
               });
     
    // FIX #41: Rotate between top 3 candidates on retry for path diversity
    // This allows different relay paths to be attempted if first choice fails
    size_t topN = std::min((size_t)3, candidates.size());
    size_t idx = p.tries % topN;  // Rotate based on attempt number
     
     if (idx >= candidates.size()) {
         idx = 0;  // Safety fallback
     }
     
     EdgeCandidate best = candidates[idx];
     
     //fw+
     // FIX #95: Log if we're using direct neighbor as fallback
     if (best.hopsFromUs == 0) {
         LOG_INFO("DTN: Progressive relay try=%u selected=0x%x (rank %u/%u score=%.1f DIRECT NEIGHBOR FALLBACK)",
                  (unsigned)p.tries, (unsigned)best.node, (unsigned)(idx+1),
                  (unsigned)candidates.size(), best.score);
     } else {
     LOG_INFO("DTN: Progressive relay try=%u selected=0x%x (rank %u/%u score=%.1f)",
              (unsigned)p.tries, (unsigned)best.node, (unsigned)(idx+1),
              (unsigned)candidates.size(), best.score);
     }
     
     // FIX #86: Update chain limit display to 5
     LOG_INFO("DTN: Progressive relay dest=0x%x via edge node=0x%x (hops=%u score=%.1f chain=%u/%u)",
              (unsigned)dest, (unsigned)best.node, best.hopsFromUs, best.score,
              p.progressiveRelayCount + 1, 5);
     
     // Track this relay to prevent loops
     // FIX #86: Allow up to 5 progressive relay hops (was 3)
    if (p.progressiveRelayCount < 5) {
        bool alreadyRecorded = false;
        for (uint8_t j = 0; j < p.progressiveRelayCount; ++j) {
            if (p.progressiveRelays[j] == best.node) {
                alreadyRecorded = true;
                break;
            }
        }
        if (!alreadyRecorded) {
            p.progressiveRelays[p.progressiveRelayCount] = best.node;
            p.progressiveRelayCount++;
        }
    }
     
     ctrProgressiveRelays++;
     
     return best.node;
     LOG_DEBUG("DTN: chooseProgressiveRelay dla dest=0x%x, kandydaci=%u", (unsigned)dest, (unsigned)candidates.size());
}
//helper: convert DV-ETX next_hop byte (low 8 bits) to full NodeNum
NodeNum DtnOverlayModule::resolveNextHopNode(uint8_t nextHopByte) const
{
    if (!nodeDB) return 0;

    NodeNum self = nodeDB->getNodeNum();
    if ((self & 0xFFu) == nextHopByte) {
        return self;
    }

    int totalNodes = nodeDB->getNumMeshNodes();
    for (int idx = 0; idx < totalNodes; ++idx) {
        meshtastic_NodeInfoLite *ni = nodeDB->getMeshNodeByIndex(idx);
        if (!ni || ni->num == 0) continue;
        if ((ni->num & 0xFFu) == nextHopByte) {
            return ni->num;
        }
    }

    return 0; // Not found
}

// Purpose: invalidate stale routes for mobile nodes to prevent using outdated handoff candidates
// Effect: removes routes to nodes that are no longer reachable or have low confidence
void DtnOverlayModule::invalidateStaleRoutes()
{
    float mobility = fwplus_getMobilityFactor01();
    
    // Only invalidate if we're mobile (mobility > 0.3) to avoid disrupting stable nodes
    if (mobility < 0.3f) return;
    
    // Check all known FW+ nodes for stale routes
    for (auto it = fwplusVersionByNode.begin(); it != fwplusVersionByNode.end();) {
        NodeNum node = it->first;
        
        if (isRouteStale(node)) {
            LOG_INFO("DTN: Invalidating stale route to mobile node 0x%x (mobility=%.2f)", (unsigned)node, mobility);
            
            // Remove from FW+ version tracking
            it = fwplusVersionByNode.erase(it);
            
            // Also penalize route in NextHopRouter if available
            if (router) {
                if (auto nh = router->asNextHopRouter()) {
                    nh->penalizeRouteOnFailed(0, node, 0, meshtastic_Routing_Error_MAX_RETRANSMIT); // Strong penalty for stale route
                }
            }
        } else {
            ++it;
        }
    }
}

// Purpose: check if a route to a destination is stale based on mobility and time
// Returns: true if route should be considered stale and invalidated
bool DtnOverlayModule::isRouteStale(NodeNum dest) const
{
    float mobility = fwplus_getMobilityFactor01();
    
    // Get node info to check last seen time
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(dest);
    if (!node) return true; // No node info = stale
    
    // Calculate stale timeout based on mobility
    // Mobile nodes: 15-30min, stationary: 2h
    uint32_t staleTimeoutSec = 30UL * 60UL; // 30min base (in seconds)
    if (mobility < 0.3f) {
        staleTimeoutSec = 2UL * 60UL * 60UL; // 2h for stationary
    } else if (mobility > 0.7f) {
        staleTimeoutSec = 15UL * 60UL; // 15min for very mobile
    }
    
    // Check if we haven't seen this node recently (use epoch time, not uptime)
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet); // epoch seconds
    uint32_t lastSeenEpoch = node->last_heard; // epoch seconds
    
    // Handle no-epoch-time scenarios
    // PROBLEM: nowEpoch==0 → return true (stale) → invalidate ALL routes → DTN loses nodes!
    // SOLUTION: If no valid time, trust routing table (NextHopRouter manages actual liveness)
    // RATIONALE: This function is for proactive mobile route invalidation (optimization)
    //            Without time, can't determine mobility staleness → defer to routing layer
    if (nowEpoch == 0 || lastSeenEpoch == 0) {
        // No valid epoch time - can't determine staleness
        // Trust routing table (NextHopRouter will penalize dead routes via ETX)
        LOG_DEBUG("DTN: Route staleness check skipped for 0x%x (no epoch time, hops=%u)", 
                 (unsigned)dest, (unsigned)node->hops_away);
        return false; // Not stale (let routing layer handle liveness)
    }
    
    if (nowEpoch - lastSeenEpoch > staleTimeoutSec) {
        LOG_DEBUG("DTN: Route to 0x%x is stale (age=%u sec, timeout=%u sec, mobility=%.2f)", 
                 (unsigned)dest, (unsigned)(nowEpoch - lastSeenEpoch), (unsigned)staleTimeoutSec, mobility);
        return true;
    }
    
    // Check if node is too far away (hops > 3 for mobile nodes)
    if (mobility > 0.5f && node->hops_away > 3) {
        LOG_DEBUG("DTN: Route to 0x%x is too far for mobile node (hops=%u, mobility=%.2f)", 
                 (unsigned)dest, (unsigned)node->hops_away, mobility);
        return true;
    }
    
    return false;
}

// Purpose: DV-ETX gating wrapper - more permissive for mobile nodes
bool DtnOverlayModule::hasSufficientRouteConfidence(NodeNum dest) const
{
    //Guard: broadcast has no route confidence (not a unicast destination)
    if (dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA) {
        return false;
    }
    
    if (!router) return true;
    // fw+ more permissive: allow attempts even with low confidence for mobile nodes
    float mobility = fwplus_getMobilityFactor01();
    if (mobility > 0.5f) return true; // Mobile nodes: always try
    // DTN uses minimal confidence 0 (any routing hint is better than nothing)
    // This is more permissive than strict routing (confidence=1) because DTN has custody transfer
    // Even weak routing hints help avoid wrong-direction handoff
    return router->hasRouteConfidence(dest, 0);
}

// Purpose: observe OnDemand responses to discover DTN-enabled nodes
// Effect: adds DTN-enabled nodes to fwplusVersionByNode for future handoff consideration
void DtnOverlayModule::observeOnDemandResponse(const meshtastic_MeshPacket &mp)
{
    // Check if we already know this node as FW+
    NodeNum origin = getFrom(&mp);
    bool wasUnknown = !isFwplus(origin);
    
    // Decode OnDemand response
    meshtastic_OnDemand ondemand = meshtastic_OnDemand_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_OnDemand_msg, &ondemand)) {
        return;
    }
    
    // Check if this is a DTN overlay stats response
    if (ondemand.which_variant == meshtastic_OnDemand_response_tag &&
        ondemand.variant.response.response_type == meshtastic_OnDemandType_RESPONSE_DTN_OVERLAY_STATS) {
        
        auto &dtnStats = ondemand.variant.response.response_data.dtn_overlay_stats;
        if (dtnStats.has_enabled && dtnStats.enabled) {
            // This node has DTN enabled! Add it to our FW+ tracking
            LOG_INFO("DTN: Discovered DTN-enabled node 0x%x via OnDemand response", (unsigned)origin);
            
            // Add to FW+ version tracking (use current FW+ version as placeholder)
            fwplusVersionByNode[origin] = FW_PLUS_VERSION;
            
            //Clear stock marking - node has DTN enabled now
            auto itStock = stockKnownMs.find(origin);
            if (itStock != stockKnownMs.end()) {
                LOG_INFO("DTN: Clearing stock marking for node 0x%x (OnDemand shows DTN enabled)", (unsigned)origin);
                stockKnownMs.erase(itStock);
            }
            
            // Also update last seen time
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(origin);
            if (node) {
                node->last_heard = getTime();
            }
            
            // fw+ Proactively probe this DTN-enabled node to get its actual FW+ version
            // This helps us learn the real version instead of using placeholder
            
            // Global rate limiter
            if (!isGlobalProbeCooldownActive()) {
                uint32_t nowMs = millis();
                auto it = lastTelemetryProbeToNodeMs.find(origin);
                if (it == lastTelemetryProbeToNodeMs.end() || (nowMs - it->second) >= configTelemetryProbeCooldownMs) {
                    if (!(airTime && !airTime->isTxAllowedChannelUtil(true))) {
                        LOG_DEBUG("DTN: Proactively probing DTN-enabled node 0x%x for FW+ version", (unsigned)origin);
                        maybeProbeFwplus(origin);
                        lastTelemetryProbeToNodeMs[origin] = nowMs;
                        updateGlobalProbeTimestamp();
                    }
                }
            } else {
                LOG_DEBUG("DTN: OnDemand probe blocked by global rate limiter (need %u sec)", 
                         getGlobalProbeCooldownRemainingSec());
            }
            
            // fw+ NEW: If this is a newly discovered DTN node, check if any pending messages can benefit
            if (wasUnknown) {
                LOG_INFO("DTN: New DTN node discovered - checking pending messages for potential handoff");
                checkPendingMessagesForHandoff(origin);
            }
        }
    }
}

// Purpose: calculate base delay for scheduling
uint32_t DtnOverlayModule::calculateBaseDelay(const meshtastic_FwplusDtnData &d, const Pending &p) const
{
    //Fast path for direct neighbors - much shorter delays for hop=0
    if (isDirectNeighbor(d.orig_to)) {
        uint32_t base = 50; // 50ms base for direct neighbors (vs 2000ms for others)
        
        // Minimal jitter for direct neighbors
        bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
        if (isFromSource && p.tries == 0) {
            base += (uint32_t)random(50); // 0-50ms jitter (vs 200-600ms)
        }
        
        // Minimal grace window for direct neighbors
        if (!isFromSource && p.tries == 0 && configGraceAckMs) {
            base += 25; // 25ms grace (vs 500ms)
        }
        
        return base;
    }
    
    // Standard delay for multi-hop destinations
    uint32_t base = configInitialDelayBaseMs ? configInitialDelayBaseMs : 8000;
    
    // Immediate-from-source: minimal jitter for fast first attempt
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    if (isFromSource && p.tries == 0) {
        base = 50 + (uint32_t)random(101); // 50..150 ms - faster first send for source
    }
    
    // Apply grace window only for non-self-origin
    if (!isFromSource && p.tries == 0 && configGraceAckMs) {
        if (base < configGraceAckMs) base = configGraceAckMs;
    }
    
    // If destination is our direct neighbor and suppression is enabled, be extra conservative
    if (configSuppressIfDestNeighbor && isDirectNeighbor(d.orig_to)) {
        base += configGraceAckMs ? configGraceAckMs : 1500;
    }
    
    return base;
}

// Purpose: calculate topology-aware delay for far nodes
//fw+
// FIX #97: Disable topology delay - it blocks custody chain forwarding!
// PROBLEM: Node 15→13 (hops=6 > maxRings=4) gets 60s delay before first custody send
//          Timeline: capture 12:02:51 → schedule next=78s (base+routing+TOPOLOGY 60s!)
//          Result: Pakiet czeka 60s zamiast być natychmiast przekazany custody chain
// ROOT CAUSE: Topology delay assumes "wait for closer nodes" model (passive overhearing)
//             BUT DTN custody is ACTIVE forwarding - we MUST send quickly to custody chain!
// SOLUTION: Return 0 for custody chain forwarding (topology delay only for source discovery)
//           This allows custody packets to be forwarded immediately regardless of distance
// BENEFIT: Multi-hop custody works correctly, packets don't stall for 60s at each hop
uint32_t DtnOverlayModule::calculateTopologyDelay(const meshtastic_FwplusDtnData &d) const
{
    // Topology delay DISABLED for custody chain forwarding
    // Rationale: DTN relies on active custody handoff, not passive "wait for closer nodes"
    // Far nodes in custody chain must forward quickly, not wait for 60s!
    return 0;
    
    // ORIGINAL CODE (disabled):
    // if (configMaxRingsToAct == 0) return 0;
    // uint8_t hopsToDest = getHopsAway(d.orig_to);
    // if (hopsToDest == 255 || hopsToDest <= configMaxRingsToAct) return 0;
    // if (d.ttl_remaining_ms > 0) {
    //     uint32_t mustWait = (d.ttl_remaining_ms * configFarMinTtlFracPercent) / 100;
    //     const uint32_t MAX_FAR_DELAY_MS = 60000;
    //     return (mustWait > MAX_FAR_DELAY_MS) ? MAX_FAR_DELAY_MS : mustWait;
    // }
    // return 5000;
}

// Purpose: calculate mobility-aware election slot timing
uint32_t DtnOverlayModule::calculateMobilitySlot(uint32_t id, const meshtastic_FwplusDtnData &d, const Pending &p) const
{
    //Source node with first attempt: NO slotting - immediate send
    bool isFromSource = (d.orig_from == nodeDB->getNodeNum());
    if (isFromSource && p.tries == 0) {
        return 0; // No slot delay for source's first attempt
    }
    
    //Fast path for direct neighbors - minimal slotting
    if (isDirectNeighbor(d.orig_to)) {
        uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
        uint32_t slot = rank % 2; // 2 slots for direct neighbors
        return slot * 25 + (uint32_t)random(25); // 25ms slots for direct neighbors
    }
    
    float mobility = fwplus_getMobilityFactor01();
    uint32_t slots = 8;
    uint32_t slotLen = 400;
    
    if (mobility > 0.5f) {
        slots = 6;
        slotLen = 300;
    }
    
    uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
    uint32_t slot = rank % slots;
    
    return slot * slotLen + (uint32_t)random(slotLen);
}

// Purpose: apply per-destination spacing constraints
uint32_t DtnOverlayModule::applyPerDestinationSpacing(uint32_t target, NodeNum dest, float mobility) const
{
    auto itLast = lastDestTxMs.find(dest);
    if (itLast == lastDestTxMs.end()) return target;
    
    uint32_t spacing = configPerDestMinSpacingMs ? configPerDestMinSpacingMs : 30000;
    // fw+ reduce spacing when mobile (down to ~60%) to react faster
    spacing = (uint32_t)((float)spacing * (1.0f - 0.4f * mobility));
    
    if (target < itLast->second + spacing) {
        target = itLast->second + spacing + (uint32_t)random(500);
    }
    
    return target;
}

// Purpose: check if fallback should be used for this pending item
bool DtnOverlayModule::shouldUseFallback(const Pending &p) const
{
    return !isFwplus(p.data.orig_to) && p.data.allow_proxy_fallback;
}

// Purpose: try near-destination fallback for close targets
bool DtnOverlayModule::tryNearDestinationFallback(uint32_t id, Pending &p)
{
    // FIX #56: Don't use near-dest fallback for intermediates!
    // PROBLEM: Intermediate sees 1 hop to dest → fallback → breaks custody chain!
    // EXAMPLE: Test 2 (A→D via B):
    //   - A (source) sees 2 hops to D → DTN custody → B (OK)
    //   - B (intermediate) sees 1 hop to D → OLD: fallback (FAIL) NEW: forward custody (OK)
    // SOLUTION: Only source can use near-dest fallback, intermediates must forward custody
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    if (!isFromSource) {
        return false;  // Intermediates: don't break custody chain!
    }
    
    uint8_t hops = getHopsAway(p.data.orig_to);
    if (hops != 255 && hops <= 1 && shouldUseFallback(p)) {
        //FIX #145: Always return true after sendProxyFallback (pending is erased, p is invalid) - part 3
        sendProxyFallback(id, p);
        return true; // Always return true - pending is erased, p is invalid
    }
    return false;
}


// Purpose: try fallback for known stock destinations or TTL tail
bool DtnOverlayModule::tryKnownStockFallback(uint32_t id, Pending &p)
{
    // FIX #58: Don't use stock fallback for intermediates!
    // PROBLEM: Intermediate receives custody A→D (stock dest), but fallback breaks custody chain
    // EXAMPLE: Test 2 (A→D via B):
    //   - A (source) sees 2 hops to D → DTN custody → B (OK)
    //   - B (intermediate) sees D is stock → OLD: fallback (FAIL) NEW: forward custody (OK)
    // SOLUTION: Only source can use stock fallback, intermediates must forward custody
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    if (!isFromSource) {
        return false;  // Intermediates: don't break custody chain!
    }
    
    bool destKnownStock = isDestKnownStock(p.data.orig_to);
    
    // Check if we're in TTL tail (helper handles all validity checks)
    bool inTail = configLateFallback && isInTtlTail(p.data, configFallbackTailPercent);
    
    if ((destKnownStock || inTail) && shouldUseFallback(p)) {
        if (inTail && configProbeFwplusNearDeadline) {
            // Global rate limiter
            if (!isGlobalProbeCooldownActive()) {
                maybeProbeFwplus(p.data.orig_to);
                updateGlobalProbeTimestamp();
            } else {
                LOG_DEBUG("DTN: TTL tail probe blocked by global rate limiter (need %u sec)", 
                         getGlobalProbeCooldownRemainingSec());
            }
        }
        //FIX #145: Always return true after sendProxyFallback (pending is erased, p is invalid) - part 4
        sendProxyFallback(id, p);
        return true; // Always return true - pending is erased, p is invalid
    }
    return false;
}
// Purpose: try fallback for unresponsive FW+ destinations
// Returns: true if fallback was attempted
// fw+ New mechanism to handle FW+ destinations that don't respond despite being marked as DTN-capable.
// This catches cases where: version changed, node is offline/unreachable, or has incompatible DTN implementation.
bool DtnOverlayModule::tryFwplusUnresponsiveFallback(uint32_t id, Pending &p)
{
    // Skip if feature disabled
    if (!configFwplusUnresponsiveFallback) return false;
    
    // Skip if already triggered fallback for this message
    if (p.fallbackTriggered) return false;
    
    // Skip if destination is not FW+ (normal fallback logic handles stock nodes)
    if (!isFwplus(p.data.orig_to)) return false;
    
    // Skip if proxy fallback is not allowed
    if (!p.data.allow_proxy_fallback) return false;
    
    // Check if we have enough failed attempts
    if (p.dtnFailedAttempts < configFwplusFailureThreshold) return false;
    
    // Check if enough time has passed since last attempt to conclude unresponsiveness
    if (p.lastDtnAttemptMs == 0) return false;
    uint32_t timeSinceLastAttempt = millis() - p.lastDtnAttemptMs;
    if (timeSinceLastAttempt < configFwplusResponseTimeoutMs) return false;
    
    // All conditions met - destination is FW+ but unresponsive, try native DM fallback
    LOG_WARN("DTN: FW+ dest 0x%x unresponsive after %u attempts over %u ms - trying native DM fallback",
             (unsigned)p.data.orig_to, (unsigned)p.dtnFailedAttempts, (unsigned)timeSinceLastAttempt);
    
    // Mark that we've triggered fallback to avoid repeating
    p.fallbackTriggered = true;
    
    //FIX #141: Copy p.data BEFORE sendProxyFallback (p will be invalid after erase!)
    NodeNum destCopy = p.data.orig_to;  // Save for LOG after erase
    
    // Send native DM fallback
    //FIX #145: Always return true after sendProxyFallback (pending is erased, p is invalid)
    sendProxyFallback(id, p);
    LOG_INFO("DTN: Native DM fallback sent to unresponsive FW+ dest 0x%x", (unsigned)destCopy);  // Use copy!
    ctrFwplusUnresponsiveFallbacks++; //increment counter for diagnostics
    
    // REMOVED: Don't mark as stock here - unresponsiveness might be temporary (congestion, routing)
    // Stock marking only via native ACK/NAK (explicit confirmation)
    
    return true; // Always return true - pending is erased, p is invalid
}

//fw+
// FIX #96: Last resort fallback - find any direct FW+ neighbor
// PROBLEM: When chooseProgressiveRelay() returns 0 (no edge nodes), we send to=dest directly
//          Intermediate nodes see mp.to=dest and don't take custody (mp.to != their NodeNum)
//          Result: packets broadcast through mesh but never enter custody chain
// SOLUTION: Use ANY direct FW+ neighbor as last resort, even if already in relay chain
//           This ensures packet enters custody chain instead of failed direct attempt
// BENEFIT: 1 failed custody transfer >> 0 custody transfers (direct attempt)
NodeNum DtnOverlayModule::findDirectFwplusNeighbor(const Pending &p)
{
    // Find any direct neighbor (hops=0) that is FW+
    // NOTE: Using C++11 iterator for ESP32 compatibility (no C++17 structured bindings)
    for (auto it = fwplusSeenMs.begin(); it != fwplusSeenMs.end(); ++it) {
        NodeNum node = it->first;
        
        //FIX #114: Skip self (prevent self-handoff loop)
        // PROBLEM: Node 1 tried to handoff to itself (0x11 -> 0x11) when no edge nodes found
        // ROOT CAUSE: No check for node == ourNodeNum in findDirectFwplusNeighbor()
        // SOLUTION: Skip self when searching for FW+ neighbors
        if (node == nodeDB->getNodeNum()) continue;
        
        // Skip destination (we want intermediate)
        if (node == p.data.orig_to) continue;
        
        // Check if it's a direct neighbor
        uint8_t hops = getHopsAway(node);
        if (hops != 0) continue;
        
        // Verify still FW+
        if (!isFwplus(node)) continue;
        
        // Check if reachable
        if (!isNodeReachable(node)) continue;
        
        LOG_INFO("DTN: Last resort - using direct FW+ neighbor 0x%x (no edge nodes available)", (unsigned)node);
        return node;
    }
    
    return 0; // No direct FW+ neighbors found
}
//fw+
// Purpose: select the best forward target for DTN packet
NodeNum DtnOverlayModule::selectForwardTarget(Pending &p)
{
     NodeNum target = p.data.orig_to;
     bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
     //tighten stale detection for receipts (shorter reachability window)
     uint32_t reachabilityOverride = p.isReceiptCustody ? configReceiptMaxNodeAgeSec : 0;
     
     //FIX #116: Return path optimization - use forceEdge if set
     // PROBLEM: DELIVERED receipts need to return via same route as original message
     // SOLUTION: emitReceipt sets forceEdge to reverse custody_path first hop
     // BENEFIT: Receipt routing bypasses forward path logic (no topology guessing)
     if (p.forceEdge != 0) {
         bool forcedReachable = isNodeReachable(p.forceEdge, reachabilityOverride);
         if (forcedReachable) {
             LOG_INFO("DTN: Using forced edge 0x%x for return path optimization (orig_to=0x%x)",
                     (unsigned)p.forceEdge, (unsigned)p.data.orig_to);
             return p.forceEdge;
         } else {
             LOG_WARN("DTN: Forced edge 0x%x unreachable or stale - clearing for id=0x%x",
                     (unsigned)p.forceEdge, (unsigned)p.data.orig_id);
             p.forceEdge = 0; // Clear invalid forceEdge
         }
     }
     
     if (configEnableFwplusHandoff) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        //Track router confidence so we can reuse progressive relay when native routing lacks next hop
        bool noRouterNextHop = (hopsToDest != 255 && !hasRouterNextHop(p.data.orig_to));
         
         // ADAPTIVE ROUTING: Check if we have a learned successful chain for this destination
         // Priority: Learned chain > Progressive relay > Direct
         auto itChain = learnedChainsByDest.find(p.data.orig_to);
        bool hasLearnedChain = (itChain != learnedChainsByDest.end() && 
                                 (millis() - itChain->second.learnedMs) < configChainCacheTtlMs &&
                                 isFwplus(itChain->second.firstHop) &&
                                 isNodeReachable(itChain->second.firstHop, reachabilityOverride));
         
        if (hasLearnedChain) {
            // Validate learned chain link health before using!
            // PROBLEM: Learned chain points to 0x13, but link is UNHEALTHY (5 failures)
            //          Using unhealthy link causes retries, bounces, delays (3min vs 2min)
            // SOLUTION: Check link health before using learned chain
            //           If unhealthy, invalidate cache and use progressive relay instead
            // BENEFIT: Adaptive routing avoids dead links, finds working alternatives
            
            NodeNum learnedFirstHop = itChain->second.firstHop;
            bool isHealthy = isLinkHealthy(learnedFirstHop);
            
            //FIX #110: Check custody_path loop before using learned chain
            // PROBLEM: Node 7 learned "0x13 works for dest 0x1d"
            //          BUT custody_path = [0x11->0x13->0x17]
            //          Node 7 sends back to 0x13 → LOOP! (FAIL)
            // ROOT CAUSE: Learned chain doesn't check if firstHop is already in custody_path
            // SOLUTION: Check custody_path before using learned firstHop
            // BENEFIT: Prevents ping-pong custody loops (0x13 ↔ 0x17)
            bool isInCustodyPath = false;
            for (uint8_t i = 0; i < p.data.custody_path_count; i++) {
                if (p.data.custody_path[i] == learnedFirstHop) {
                    isInCustodyPath = true;
                    break;
                }
            }
            
            if (isInCustodyPath) {
                // Learned first-hop is already in custody_path → would create loop!
                LOG_WARN("DTN: Learned chain first-hop 0x%x ALREADY in custody_path - invalidating to prevent loop",
                         (unsigned)learnedFirstHop);
                learnedChainsByDest.erase(itChain); // Remove to prevent future loops
                hasLearnedChain = false; // Use progressive relay instead
            } else if (isHealthy) {
                // Use learned first-hop from successful chain
                target = learnedFirstHop;
                uint32_t ageMin = (millis() - itChain->second.learnedMs) / 60000;
                LOG_INFO("DTN: Using learned chain first-hop 0x%x for dest 0x%x (learned %u min ago, chain_len=%u, healthy=1)",
                         (unsigned)target, (unsigned)p.data.orig_to, (unsigned)ageMin, 
                         (unsigned)itChain->second.chainLength);
            } else {
                // Learned chain is unhealthy - invalidate and use progressive relay
                LOG_WARN("DTN: Learned chain first-hop 0x%x for dest 0x%x is UNHEALTHY - invalidating cache",
                         (unsigned)learnedFirstHop, (unsigned)p.data.orig_to);
                learnedChainsByDest.erase(itChain); // Remove stale cache
                hasLearnedChain = false; // Mark as invalid to trigger alternative logic below
            }
        }
        
        
        // If learned chain was invalid or unhealthy, proceed with route-based selection
        else if (!hasLearnedChain && p.isReceiptCustody && (p.tries > 0 || noRouterNextHop)) {
            NodeNum edgeNode = p.cachedProgressiveRelay;
            if (edgeNode == 0) {
                edgeNode = chooseProgressiveRelay(p.data.orig_to, p);
            }

            if (edgeNode != 0) {
                target = edgeNode;
                LOG_WARN("DTN: Receipt custody id=0x%x switching to progressive relay 0x%x after direct attempt",
                         (unsigned)p.data.orig_id, (unsigned)target);
            } else {
                NodeNum neighbor = findDirectFwplusNeighbor(p);
                if (neighbor != 0) {
                    target = neighbor;
                    LOG_INFO("DTN: Receipt custody id=0x%x - using direct neighbor 0x%x after relapse",
                             (unsigned)p.data.orig_id, (unsigned)target);
                } else {
                    target = p.data.orig_to;
                    LOG_WARN("DTN: Receipt custody id=0x%x could not find progressive relay, retrying direct",
                             (unsigned)p.data.orig_id);
                }
            }
        }
        else if (!hasLearnedChain && hopsToDest == 255) {
            
            if (p.isReceiptCustody) {
                target = p.data.orig_to; // Direct attempt using routing table
                LOG_INFO("DTN: Receipt custody id=0x%x - using direct routing to 0x%x (skip progressive relay)",
                         (unsigned)p.data.orig_id, (unsigned)target);
             } else {
                 // === CASE 1: Unknown Route (hops=255) - Use Progressive Relay ===
                 LOG_INFO("DTN: Destination 0x%x unknown (hops=255) - attempting progressive relay",
                          (unsigned)p.data.orig_to);
                 
                 //FIX #178: Use cached progressive relay choice (prevent double-call)
                 NodeNum edgeNode = p.cachedProgressiveRelay; // Use cached value from tryForward
                 if (edgeNode != 0) {
                     target = edgeNode;
                     LOG_INFO("DTN: Progressive relay to edge node 0x%x (unknown dest 0x%x)",
                              (unsigned)target, (unsigned)p.data.orig_to);
                 } else {
                     //fw+
                     // FIX #96: Try direct FW+ neighbor as last resort
                     NodeNum neighbor = findDirectFwplusNeighbor(p);
                     if (neighbor != 0) {
                         target = neighbor;
                         LOG_INFO("DTN: Using direct neighbor 0x%x (dest 0x%x unknown, no edge nodes)",
                              (unsigned)target, (unsigned)p.data.orig_to);
                 } else {
                     LOG_WARN("DTN: No progressive relay available for unknown dest 0x%x - direct attempt",
                              (unsigned)p.data.orig_to);
                     // Keep target as orig_to (direct delivery attempt - will likely use fallback)
                     }
                 }
             }
         }
        // === CASE 2: Too Distant (hops > HOP_RELIABLE=3) - Use Progressive Relay ===
        else if (!hasLearnedChain && hopsToDest > HOP_RELIABLE) {
            if (p.isReceiptCustody) {
                target = p.data.orig_to; // Direct attempt using routing table
                LOG_INFO("DTN: Receipt custody id=0x%x - using direct routing to 0x%x (far, skip progressive relay)",
                         (unsigned)p.data.orig_id, (unsigned)target);
            } else {
                LOG_INFO("DTN: Destination 0x%x too far (hops=%u > reliable=%u) - attempting progressive relay",
                         (unsigned)p.data.orig_to, hopsToDest, HOP_RELIABLE);
                 
                 //FIX #178: Use cached progressive relay choice (prevent double-call)
                 NodeNum edgeNode = p.cachedProgressiveRelay; // Use cached value from tryForward
                 if (edgeNode != 0) {
                     target = edgeNode;
                     LOG_INFO("DTN: Progressive relay to edge node 0x%x (distant dest 0x%x)",
                              (unsigned)target, (unsigned)p.data.orig_to);
                 } else {
                     // No progressive relay available - try normal handoff as fallback
                     NodeNum cand = chooseHandoffTarget(p.data.orig_to, 0, p);
                     if (cand != 0 && isValidHandoffCandidate(cand, p.data.orig_to, p)) {
                         target = cand;
                         LOG_INFO("DTN: Using handoff target 0x%x for distant dest (no progressive relay)",
                                  (unsigned)target);
                     } else {
                         //fw+
                         // FIX #96: Try direct FW+ neighbor as last resort
                         NodeNum neighbor = findDirectFwplusNeighbor(p);
                         if (neighbor != 0) {
                             target = neighbor;
                             LOG_INFO("DTN: Using direct neighbor 0x%x (dest 0x%x distant, no relay/handoff)",
                                      (unsigned)target, (unsigned)p.data.orig_to);
                         } else {
                             LOG_WARN("DTN: No relay available for distant dest 0x%x - direct attempt",
                                      (unsigned)p.data.orig_to);
                         }
                     }
                 }
             }
         }
        //Honor progressive relay choice when Router lacks next hop (known-route case)
        else if (!hasLearnedChain && isFromSource && noRouterNextHop) {
            NodeNum edgeNode = p.cachedProgressiveRelay;
            if (edgeNode != 0) {
                target = edgeNode;
                LOG_INFO("DTN: Progressive relay (no Router next hop) to edge node 0x%x (dest 0x%x hops=%u)",
                         (unsigned)target, (unsigned)p.data.orig_to, (unsigned)hopsToDest);
            } else {
                NodeNum neighbor = findDirectFwplusNeighbor(p);
                if (neighbor != 0) {
                    target = neighbor;
                    LOG_INFO("DTN: Using direct neighbor 0x%x (Router lacks next hop to 0x%x)",
                             (unsigned)target, (unsigned)p.data.orig_to);
                } else {
                    LOG_WARN("DTN: Router lacks next hop to dest 0x%x - proceeding with direct attempt",
                             (unsigned)p.data.orig_to);
                }
            }
        }
        // === CASE 3: Known Route within HOP_RELIABLE ===
        // FIX #57: Intermediates should forward directly, NOT use handoff!
         // PROBLEM: Intermediate B receives custody A→D, but handoff logic sends to E instead of D
         // REASON: Handoff is for SOURCE to choose carrier, not for INTERMEDIATE to reroute!
         // SOLUTION: Only source can use handoff for known routes, intermediates forward directly
         else if (!hasLearnedChain && hopsToDest >= configHandoffMinRing && isFromSource) {
             // SOURCE: Try to find handoff candidate
             NodeNum cand = chooseHandoffTarget(p.data.orig_to, 0, p);
             if (cand != 0 && isValidHandoffCandidate(cand, p.data.orig_to, p)) {
                 target = cand;
                 LOG_DEBUG("DTN: Source using handoff target: 0x%x (hops=%u)", (unsigned)target, (unsigned)hopsToDest);
             } else {
                 LOG_DEBUG("DTN: No valid handoff candidates available for source, using direct");
                 // Keep target as orig_to (direct delivery attempt)
             }
        } else if (!hasLearnedChain && hopsToDest >= configHandoffMinRing && !isFromSource) {
            // Intermediate ALWAYS uses edge/relay, NEVER direct destination!
            // PROBLEM: "Intermediate forwarding directly to dest 0x11 (hops=2)"
            //          → Router changes mp.to from 0x11 to next_hop (0x1c)
            //          → Node1 receives mp.to=0x1c (thinks "relay - not for us")
            //          → Node1 sends PROGRESSED, not DELIVERED! (FAIL)
            // ROOT CAUSE: DTN custody requires mp.to=edge (who will take custody next)
            //             NOT mp.to=destination (Router will change it to next_hop anyway!)
            // SOLUTION: Intermediate ALWAYS chooses edge/relay node (progressive or handoff)
            //           NEVER forwards "directly" to destination (even if hops <= HOP_RELIABLE)
            // BENEFIT: Custody chain works correctly, packets arrive with correct mp.to
            
            // Try progressive relay first (best for unknown/distant)
            NodeNum edgeNode = chooseProgressiveRelay(p.data.orig_to, p);
            if (edgeNode != 0) {
                target = edgeNode;
                LOG_INFO("DTN: Intermediate using progressive relay to 0x%x (dest 0x%x hops=%u)",
                         (unsigned)target, (unsigned)p.data.orig_to, (unsigned)hopsToDest);
            } else {
                //FIX #109: Intermediate Intelligence - Stock Detection
                // PROBLEM: No edge nodes found → intermediate can't forward via DTN
                //          Should intermediate send FAILED or try handoff?
                // SOLUTION: Check if dest is known Stock (NodeInfo but NO beacon)
                //           If Stock → send FAILED receipt (DTN can't help)
                //           If unknown → try handoff/direct (maybe source knows something)
                
                // Check if we KNOW dest is Stock (NodeInfo received but NO DTN beacon)
                auto fwIt = fwplusVersionByNode.find(p.data.orig_to);
                bool destIsFwPlus = (fwIt != fwplusVersionByNode.end());
                
                if (!destIsFwPlus) {
                    // Dest is NOT in FW+ list - check if we saw NodeInfo
                    auto *nodeInfo = nodeDB->getMeshNode(p.data.orig_to);
                    if (nodeInfo && nodeInfo->has_user && nodeInfo->last_heard > 0) {
                        uint32_t nodeAge = (getValidTime(RTCQualityFromNet) - nodeInfo->last_heard);
                        if (nodeAge < 12 * 60 * 60) { // < 12h (same as isNodeReachable)
                            // We saw this node recently, but NO DTN beacon → Stock!
                            LOG_WARN("DTN: Intermediate detected dest 0x%x is Stock (NodeInfo age=%u sec, no beacon)",
                                     (unsigned)p.data.orig_to, (unsigned)nodeAge);
                            
                            // Mark for FAILED receipt (will be sent by tryForward when target=0)
                            // Return 0 = no valid DTN target, tryForward will handle cleanup
                            return 0;
                        }
                    }
                }
                
                // Dest is FW+ or completely unknown - try handoff/direct
                NodeNum cand = chooseHandoffTarget(p.data.orig_to, 0, p);
                if (cand != 0 && isValidHandoffCandidate(cand, p.data.orig_to, p)) {
                    target = cand;
                    LOG_INFO("DTN: Intermediate using handoff target 0x%x (dest 0x%x hops=%u)",
                             (unsigned)target, (unsigned)p.data.orig_to, (unsigned)hopsToDest);
                } else {
                    LOG_WARN("DTN: Intermediate no relay available for dest 0x%x (hops=%u) - keeping in pending",
                             (unsigned)p.data.orig_to, (unsigned)hopsToDest);
                    // Keep target as orig_to (maybe route discovery helps later)
                }
            }
        }
     }
     
     return target;
}

void DtnOverlayModule::setPriorityForTailAndSource(meshtastic_MeshPacket *mp, const Pending &p, bool isFromSource)
{
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    if (p.data.ttl_remaining_ms == 0) return; // No TTL - skip priority boost
    
    // Check if we're in TTL tail (helper handles valid time checks)
    bool inTail = isInTtlTail(p.data, configFallbackTailPercent);
    
    bool nearDst = false;
    if (configTailEscalateMaxRing > 0) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        nearDst = (hopsToDest != 255 && hopsToDest <= configTailEscalateMaxRing);
    }
    
    // Escalate to DEFAULT priority in TTL tail when near destination or source
    if (inTail && (nearDst || isFromSource)) {
        mp->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    }
}

void DtnOverlayModule::applyForeignCarrySuppression(uint32_t id, Pending &p)
{
    //Adaptive suppression based on mesh conditions:
    // - Longer if we see strong carriers (high propagation confidence)
    // - Longer if packet already traveled far (low hop_limit remaining)
    // - Shorter if we're mobile (topology changes quickly)
    
    uint32_t suppressMs = configSuppressMsAfterForeign;
    float mobility = fwplus_getMobilityFactor01();
    
    // Reduce suppression for mobile nodes (topology changes fast)
    if (mobility > 0.5f) {
        suppressMs = (uint32_t)((float)suppressMs * 0.6f); // 60% for mobile
    }
    
    // Note: We don't have access to RSSI/hop_limit here from stored Pending
    // Future: could track per-id statistics (seen_count, avg_rssi) for smarter suppression
    
    uint32_t postpone = millis() + suppressMs + (uint32_t)random(500);
    if (p.nextAttemptMs < postpone) p.nextAttemptMs = postpone;
    LOG_DEBUG("DTN suppress id=0x%x after foreign carry, next in %u ms (mobility=%.2f)", 
              id, (unsigned)(p.nextAttemptMs - millis()), mobility);
}

void DtnOverlayModule::applyNearDestExtraSuppression(Pending &p, NodeNum dest)
{
    uint8_t hopsToDest = getHopsAway(dest);
    if (hopsToDest != 255 && hopsToDest <= 2) {
        // Stronger suppression when immediately near destination to prevent duplicate deliveries
        uint32_t longerSuppress = (hopsToDest <= 1) ? (configSuppressMsAfterForeign * 3)
                                                    : (configSuppressMsAfterForeign * 2);
        uint32_t postpone = millis() + longerSuppress + (uint32_t)random(1000);
        if (p.nextAttemptMs < postpone) p.nextAttemptMs = postpone;
        LOG_DEBUG("DTN near-dest extra suppress for %u ms", (unsigned)longerSuppress);
    }
}

// Purpose: check if any known DTN nodes can help reach destination
// Strategy: DTN node is helpful if it's closer to destination than we are, on path to destination,
//           or can provide alternative route through mesh topology
bool DtnOverlayModule::canDtnHelpWithDestination(NodeNum dest) const
{
    if (fwplusVersionByNode.empty()) return false;
    
    //Paranoid guard: should never be called with broadcast, but double-check
    if (dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA) {
        LOG_WARN("DTN: canDtnHelpWithDestination called with broadcast dest - rejecting");
        return false;
    }
    
    // Get our distance to destination
    uint8_t ourHopsToDest = getHopsAway(dest);
    
    LOG_DEBUG("DTN canHelp check: dest=0x%x ourHops=%u fwplus_count=%u", 
              (unsigned)dest, (unsigned)ourHopsToDest, (unsigned)fwplusVersionByNode.size());
    
    // If we don't know route to dest, DTN might help with discovery
    if (ourHopsToDest == 255) {
        LOG_INFO("DTN: Unknown route to 0x%x - DTN may help with discovery", (unsigned)dest);
        return true;
    }
    
    // FIX #48: Change threshold from >=3 to >=2 hops for better DTN coverage
    // Stock routing: excellent for 0-1 hops (~90% success)
    // DTN custody: helps for 2+ hops when DTN nodes exist on path
    // This is the "im dalej w las tym wiecej drzew" scenario - multi-hop reliability degrades
    if (ourHopsToDest >= 2) {
        LOG_INFO("DTN: Dest 0x%x is %u hops away (>=2) - using DTN custody", 
                 (unsigned)dest, (unsigned)ourHopsToDest);
        return true;
    }
    
    // Check if any known DTN node can help reach destination
    for (const auto& kv : fwplusVersionByNode) {
        NodeNum dtnNode = kv.first;
        // uint16_t version = kv.second; // unused for now
        
        LOG_DEBUG("DTN canHelp: checking FW+ node 0x%x", (unsigned)dtnNode);
        
        if (dtnNode == dest) {
            // Destination itself is DTN-enabled!
            LOG_INFO("DTN: Destination 0x%x is DTN-enabled!", (unsigned)dest);
            return true;
        }
        
        // Get DTN node's distance from us and from destination
        uint8_t hopsToNode = getHopsAway(dtnNode);
        LOG_DEBUG("DTN canHelp: FW+ 0x%x hopsToNode=%u", (unsigned)dtnNode, (unsigned)hopsToNode);
        
        if (hopsToNode == 255) {
            LOG_DEBUG("DTN canHelp: skip 0x%x - unreachable", (unsigned)dtnNode);
            continue; // Can't reach this DTN node
        }
        
        // Check if DTN node is reachable and not too far
        if (!isNodeReachable(dtnNode)) {
            LOG_DEBUG("DTN canHelp: skip 0x%x - not reachable (stale)", (unsigned)dtnNode);
            continue;
        }
        
        // Strategy 1: DTN node is closer to destination than we are
        // This works for nodes on the direct path
        meshtastic_NodeInfoLite *dtnNodeInfo = nodeDB->getMeshNode(dtnNode);
        if (dtnNodeInfo) {
            // We can't directly query another node's routing table, but we can use heuristics:
            // If the DTN node is much closer to us than the destination is, it might be on path
            // This catches "side branch" nodes that can provide alternative routes
            
            // Relaxed heuristic: DTN node within reasonable distance (not just half)
            // Example: dest at 7 hops, DTN at 3 hops can still help significantly
            if (hopsToNode <= 4 && hopsToNode < ourHopsToDest) {
                LOG_DEBUG("DTN: Node 0x%x (hops=%u) may help reach 0x%x (hops=%u) - reachable and closer", 
                         (unsigned)dtnNode, (unsigned)hopsToNode, (unsigned)dest, (unsigned)ourHopsToDest);
                return true;
            }
            
            // Strategy 2: For far destinations (>4 hops), any reachable DTN node might help
            // as it can store-carry-forward through different mesh branches
            if (ourHopsToDest > 4) {
                LOG_DEBUG("DTN: Node 0x%x may help reach far dest 0x%x (hops=%u) via store-carry-forward", 
                         (unsigned)dtnNode, (unsigned)dest, (unsigned)ourHopsToDest);
                return true;
            }
            
            // Strategy 3: If we have route confidence to destination but DTN node is reachable,
            // DTN can provide redundancy and alternative path (useful for mobility)
            if (router && router->hasRouteConfidence(dest, 1) && hopsToNode <= 3) {
                LOG_DEBUG("DTN: Node 0x%x may provide redundant path to 0x%x", 
                         (unsigned)dtnNode, (unsigned)dest);
                return true;
            }
        }
    }
    
    LOG_DEBUG("DTN: No DTN nodes can help reach 0x%x - using native DM", (unsigned)dest);
    return false;
}
// Purpose: check if any pending messages can benefit from newly discovered DTN node
// Effect: may trigger re-evaluation of pending messages that were using native DM
void DtnOverlayModule::checkPendingMessagesForHandoff(NodeNum newDtnNode)
{
    if (!configEnabled) return;
    
    uint8_t hopsToNewNode = getHopsAway(newDtnNode);
    if (hopsToNewNode == 255) {
        LOG_DEBUG("DTN: Unknown route to new DTN node 0x%x - cannot evaluate handoff", (unsigned)newDtnNode);
        return;
    }
    
    LOG_INFO("DTN: Evaluating pending messages for potential handoff to new DTN node 0x%x (hops=%u)", 
             (unsigned)newDtnNode, (unsigned)hopsToNewNode);
    
    // Iterate through pending messages
    for (auto& kv : pendingById) {
        uint32_t id = kv.first;
        Pending& pending = kv.second;
        
        // Skip if message is from us and already being handled
        bool isFromUs = (pending.data.orig_from == nodeDB->getNodeNum());
        if (!isFromUs) continue; // Only consider messages we originated
        
        // Skip if destination is direct neighbor (native DM is faster)
        if (isDirectNeighbor(pending.data.orig_to)) continue;
        
        uint8_t hopsToDest = getHopsAway(pending.data.orig_to);
        if (hopsToDest == 255) {
            // Unknown destination - new DTN node might help with discovery
            LOG_INFO("DTN: Message 0x%x to unknown dest 0x%x - new DTN node may help", 
                    id, (unsigned)pending.data.orig_to);
            // Reschedule for earlier attempt
            uint32_t now = millis();
            if (pending.nextAttemptMs > now + 5000) {
                pending.nextAttemptMs = now + 2000 + (uint32_t)random(1000);
                LOG_INFO("DTN: Rescheduled message 0x%x for earlier attempt", id);
            }
            continue;
        }
        
        // Check if new DTN node is closer to destination
        // This is a simple heuristic - new node might be on path
        if (hopsToNewNode < hopsToDest) {
            LOG_INFO("DTN: Message 0x%x to dest 0x%x (hops=%u) may benefit from DTN node 0x%x (hops=%u)", 
                    id, (unsigned)pending.data.orig_to, (unsigned)hopsToDest,
                    (unsigned)newDtnNode, (unsigned)hopsToNewNode);
            
            // Reschedule for earlier attempt if currently delayed
            uint32_t now = millis();
            if (pending.nextAttemptMs > now + 5000) {
                pending.nextAttemptMs = now + 1000 + (uint32_t)random(500);
                LOG_INFO("DTN: Rescheduled message 0x%x for earlier DTN attempt", id);
            }
        }
    }
}
//==============================================================================
// ADAPTIVE PATH SELECTION & FAULT TOLERANCE - LINK HEALTH MONITORING
//==============================================================================

// Purpose: Update link health metrics after a transmission attempt
// Inputs: neighbor - next hop node, success - transmission result, 
//         snr/rssi - signal quality metrics
// Effect: Updates linkHealthMap with EWMA of metrics, tracks failure streaks
// Called: After each transmission attempt (from Router callback or periodic check)
void DtnOverlayModule::updateLinkHealth(NodeNum neighbor, bool success, float snr, float rssi)
{
    if (neighbor == 0 || neighbor == NODENUM_BROADCAST) {
        return; // Skip broadcast/invalid
    }
    
    auto& health = linkHealthMap[neighbor];
    
    // Initialize if first time
    if (health.neighbor == 0) {
        health.neighbor = neighbor;
        health.avgSnr = snr;
        health.avgRssi = rssi;
    }
    
    if (success) {
        health.successCount++;
        health.consecutiveFailures = 0;  // Reset streak
        health.lastSuccessMs = millis();
        
        // Update signal quality using EWMA (alpha=0.3 for new samples)
        health.avgSnr = (health.avgSnr * 0.7f) + (snr * 0.3f);
        health.avgRssi = (health.avgRssi * 0.7f) + (rssi * 0.3f);
        
        LOG_DEBUG("DTN LinkHealth: 0x%x SUCCESS (streak reset, snr=%.1f, total=%u)",
                  neighbor, health.avgSnr, health.successCount);
    } else {
        health.failureCount++;
        health.consecutiveFailures++;
        health.lastFailureMs = millis();
        
        LOG_DEBUG("DTN LinkHealth: 0x%x FAILURE (streak=%u, total=%u)",
                  neighbor, health.consecutiveFailures, health.failureCount);
    }
    
    health.lastHealthCheckMs = millis();
    ctrLinkHealthChecks++;
}
// Purpose: Check if a link to neighbor is healthy enough for routing
// Inputs: neighbor - node to check
// Returns: true if link is healthy, false if should be avoided
// Logic: Evaluates multiple criteria: failure streak, success rate, signal quality, staleness
bool DtnOverlayModule::isLinkHealthy(NodeNum neighbor) const
{
    if (neighbor == 0 || neighbor == NODENUM_BROADCAST) {
        return true; // Don't filter broadcast/invalid
    }
    
    auto it = linkHealthMap.find(neighbor);
    if (it == linkHealthMap.end()) {
        return true; // Unknown = assume healthy (innocent until proven guilty)
    }
    
    const auto& health = it->second;
    uint32_t now = millis();
    
    // Criterion 1: Consecutive failures (most critical)
    if (health.consecutiveFailures >= configLinkFailureThreshold) {
        LOG_DEBUG("DTN LinkHealth: 0x%x UNHEALTHY (consecutive failures=%u >= %u)",
                  neighbor, health.consecutiveFailures, configLinkFailureThreshold);
        return false;
    }
    
    // Criterion 2: Success rate (only if we have enough samples)
    uint32_t totalAttempts = health.successCount + health.failureCount;
    if (totalAttempts >= 5) {
        float successRate = (float)health.successCount / totalAttempts;
        if (successRate < configMinPathSuccessRate) {
            LOG_DEBUG("DTN LinkHealth: 0x%x UNHEALTHY (success rate=%.2f < %.2f)",
                      neighbor, successRate, configMinPathSuccessRate);
            return false;
        }
    }
    
    // Criterion 3: Signal quality (if we have measurements)
    if (health.avgSnr > 0.0f && health.avgSnr < configMinLinkSnrThreshold) {
        LOG_DEBUG("DTN LinkHealth: 0x%x UNHEALTHY (SNR=%.1f < %.1f)",
                  neighbor, health.avgSnr, configMinLinkSnrThreshold);
        return false;
    }
    
    // Criterion 4: Staleness (no success in last 5 minutes despite failures)
    bool hasRecentFailures = (now - health.lastFailureMs) < configLinkHealthWindowMs;
    bool noRecentSuccess = (now - health.lastSuccessMs) > configLinkHealthWindowMs;
    if (hasRecentFailures && noRecentSuccess && health.failureCount > 0) {
        LOG_DEBUG("DTN LinkHealth: 0x%x UNHEALTHY (stale: %u sec since last success)",
                  neighbor, (now - health.lastSuccessMs) / 1000);
        return false;
    }
    
    // All checks passed
    return true;
}

// Purpose: Get link quality score (0.0=worst, 1.0=best)
// Inputs: neighbor - node to evaluate
// Returns: Composite score based on success rate and signal quality
// Used by: Path selection to rank multiple alternatives
float DtnOverlayModule::getLinkQualityScore(NodeNum neighbor) const
{
    auto it = linkHealthMap.find(neighbor);
    if (it == linkHealthMap.end()) {
        return 0.5f; // Unknown = neutral score
    }
    
    const auto& health = it->second;
    
    // Calculate success rate component (0.0-1.0)
    float successRate = 0.5f; // Default neutral
    uint32_t totalAttempts = health.successCount + health.failureCount;
    if (totalAttempts > 0) {
        successRate = (float)health.successCount / totalAttempts;
    }
    
    // Calculate signal quality component (0.0-1.0)
    // SNR: 0dB=bad, 10dB+=excellent, normalize to 0-1
    float snrScore = 0.5f; // Default neutral
    if (health.avgSnr > 0.0f) {
        snrScore = std::min(health.avgSnr / 10.0f, 1.0f);
    }
    
    // Penalty for consecutive failures (reduces score exponentially)
    float failuresPenalty = 1.0f;
    if (health.consecutiveFailures > 0) {
        failuresPenalty = std::pow(0.5f, (float)health.consecutiveFailures); // 0.5^n
    }
    
    // Composite score: 60% success rate, 30% signal, 10% failure penalty
    float score = (successRate * 0.6f) + (snrScore * 0.3f) + (failuresPenalty * 0.1f);
    
    return std::max(0.0f, std::min(1.0f, score)); // Clamp to [0,1]
}

// Purpose: Periodic maintenance of link health data
// Effect: Removes stale entries, resets old failure streaks, logs statistics
// Called: From runPeriodicMaintenance() every 5 minutes
void DtnOverlayModule::maintainLinkHealth()
{
    uint32_t now = millis();
    uint32_t staleThreshold = 600000; // 10 minutes
    
    // Clean up stale entries and log statistics
    auto it = linkHealthMap.begin();
    while (it != linkHealthMap.end()) {
        auto& health = it->second;
        
        // Remove very old entries (no activity in 10 minutes)
        if ((now - health.lastHealthCheckMs) > staleThreshold) {
            LOG_DEBUG("DTN LinkHealth: Removing stale entry for 0x%x", health.neighbor);
            it = linkHealthMap.erase(it);
            continue;
        }
        
        // Reset consecutive failures if we've had success recently
        if (health.consecutiveFailures > 0 && 
            health.lastSuccessMs > health.lastFailureMs &&
            (now - health.lastSuccessMs) < 60000) {
            health.consecutiveFailures = 0;
            LOG_DEBUG("DTN LinkHealth: Reset failure streak for 0x%x (recent success)", 
                      health.neighbor);
        }
        
        ++it;
    }
    
    // Log summary statistics
    if (!linkHealthMap.empty()) {
        LOG_INFO("DTN LinkHealth: Monitoring %u links (checks=%u, reroutes=%u)",
                 (unsigned)linkHealthMap.size(), ctrLinkHealthChecks, ctrAdaptiveReroutes);
    }
}

//==============================================================================
// ADAPTIVE PATH SELECTION & FAULT TOLERANCE - PATH RELIABILITY LEARNING
//==============================================================================

// Purpose: Record path attempt for learning
// Inputs: firstHop - first node in path, destination - final dest, packetId - packet ID
// Effect: Creates/updates path history tracking
// Called: When forwarding packet via a specific path
void DtnOverlayModule::recordPathAttempt(NodeNum firstHop, NodeNum destination, PacketId packetId)
{
    if (firstHop == 0 || destination == 0) return;
    
    // Update packet path history
    auto& history = packetPathHistoryMap[packetId];
    history.packetId = packetId;
    history.destination = destination;
    history.recordAttempt(firstHop);
    
    // Update path reliability attempt timestamp
    auto key = std::make_pair(firstHop, destination);
    auto& reliability = pathReliabilityMap[key];
    reliability.firstHop = firstHop;
    reliability.destination = destination;
    reliability.lastAttemptMs = millis();
    
    LOG_DEBUG("DTN PathLearn: Recorded attempt id=0x%x via 0x%x->0x%x (switch=%u)",
              packetId, firstHop, destination, history.pathSwitchCount);
}
// Purpose: Update path reliability after delivery success/failure
// Inputs: firstHop - first node in path, destination - final dest, success - result
// Effect: Updates pathReliabilityMap, may block unreliable paths temporarily
// Called: When receiving RECEIPT (success) or timeout (failure)
void DtnOverlayModule::updatePathReliability(NodeNum firstHop, NodeNum destination, bool success)
{
    if (firstHop == 0 || destination == 0) return;
    
    auto key = std::make_pair(firstHop, destination);
    auto& reliability = pathReliabilityMap[key];
    
    // Initialize if first time
    if (reliability.firstHop == 0) {
        reliability.firstHop = firstHop;
        reliability.destination = destination;
    }
    
    if (success) {
        reliability.successCount++;
        reliability.lastSuccessMs = millis();
        
        // Unblock if was temporarily blocked
        if (reliability.temporarilyBlocked && millis() >= reliability.blockExpiryMs) {
            reliability.temporarilyBlocked = false;
            LOG_INFO("DTN PathLearn: Path 0x%x->0x%x UNBLOCKED (success after %.1fs)",
                     firstHop, destination, 
                     (millis() - reliability.lastFailureMs) / 1000.0f);
        }
        
        LOG_DEBUG("DTN PathLearn: Path 0x%x->0x%x SUCCESS (rate=%.2f, total=%u)",
                  firstHop, destination, reliability.getSuccessRate(), 
                  reliability.successCount);
    } else {
        reliability.failureCount++;
        reliability.lastFailureMs = millis();
        
        // Temporarily block path if too many failures
        if (reliability.failureCount >= 3 && reliability.getSuccessRate() < 0.4f) {
            reliability.temporarilyBlocked = true;
            reliability.blockExpiryMs = millis() + configPathBlockDurationMs;
            
            LOG_WARN("DTN PathLearn: Path 0x%x->0x%x BLOCKED for %u sec (failures=%u, rate=%.2f)",
                     firstHop, destination, configPathBlockDurationMs / 1000,
                     reliability.failureCount, reliability.getSuccessRate());
        } else {
            LOG_DEBUG("DTN PathLearn: Path 0x%x->0x%x FAILURE (rate=%.2f, failures=%u)",
                      firstHop, destination, reliability.getSuccessRate(), 
                      reliability.failureCount);
        }
    }
    
    ctrPathLearningUpdates++;
}

// Purpose: Check if a path is reliable based on historical data
// Inputs: firstHop - first node in path, destination - final dest
// Returns: true if path is reliable, false if should be avoided
// Used by: Path selection to filter out known-bad paths
bool DtnOverlayModule::isPathReliable(NodeNum firstHop, NodeNum destination) const
{
    auto key = std::make_pair(firstHop, destination);
    auto it = pathReliabilityMap.find(key);
    
    if (it == pathReliabilityMap.end()) {
        return true; // Unknown path = assume reliable
    }
    
    const auto& reliability = it->second;
    
    // Check if unreliable based on PathReliability::isUnreliable()
    return !reliability.isUnreliable();
}
// Purpose: Get all paths that have failed and should be avoided
// Inputs: destination - target node
// Returns: Set of first-hop nodes that lead to unreliable paths
// Used by: selectAlternativePathOnFailure() to exclude bad options
std::set<NodeNum> DtnOverlayModule::getUnreliablePaths(NodeNum destination) const
{
    std::set<NodeNum> unreliableHops;
    
    for (const auto& entry : pathReliabilityMap) {
        const auto& key = entry.first;
        const auto& reliability = entry.second;
        
        // Check if this path's destination matches
        if (key.second == destination && reliability.isUnreliable()) {
            unreliableHops.insert(key.first); // Add first hop to blacklist
        }
    }
    
    return unreliableHops;
}

// Purpose: Clean up old path learning data
// Effect: Removes stale entries, unblocks expired blocks
// Called: Periodically from runPeriodicMaintenance()
void DtnOverlayModule::maintainPathReliability()
{
    uint32_t now = millis();
    uint32_t staleThreshold = 1800000; // 30 minutes
    
    auto it = pathReliabilityMap.begin();
    while (it != pathReliabilityMap.end()) {
        auto& reliability = it->second;
        
        // Remove very old entries (no activity in 30 minutes)
        if ((now - reliability.lastAttemptMs) > staleThreshold) {
            LOG_DEBUG("DTN PathLearn: Removing stale entry 0x%x->0x%x",
                      reliability.firstHop, reliability.destination);
            it = pathReliabilityMap.erase(it);
            continue;
        }
        
        // Unblock expired temporary blocks
        if (reliability.temporarilyBlocked && now >= reliability.blockExpiryMs) {
            reliability.temporarilyBlocked = false;
            LOG_INFO("DTN PathLearn: Path 0x%x->0x%x auto-unblocked (expiry)",
                     reliability.firstHop, reliability.destination);
        }
        
        ++it;
    }
    
    // Clean up packet path history (keep only recent)
    auto histIt = packetPathHistoryMap.begin();
    while (histIt != packetPathHistoryMap.end()) {
        const auto& history = histIt->second;
        if ((now - history.lastAttemptMs) > 600000) { // 10 minutes
            histIt = packetPathHistoryMap.erase(histIt);
        } else {
            ++histIt;
        }
    }
}

//==============================================================================
// ADAPTIVE PATH SELECTION & FAULT TOLERANCE - PATH SELECTION
//==============================================================================

// Purpose: Calculate composite score for path quality
// Inputs: firstHop - first node, dest - destination, cost - DV-ETX cost, hops - hop count
// Returns: Score (0.0=worst, 1.0=best) combining multiple factors
// Used by: selectAlternativePathOnFailure() to rank paths
float DtnOverlayModule::calculatePathScore(NodeNum firstHop, NodeNum dest, float cost, uint8_t hops) const
{
    // Component 1: Link quality (40%)
    float linkScore = getLinkQualityScore(firstHop);
    
    // Component 2: Path reliability (30%)
    float pathScore = 0.5f; // Default neutral
    auto key = std::make_pair(firstHop, dest);
    auto it = pathReliabilityMap.find(key);
    if (it != pathReliabilityMap.end()) {
        pathScore = it->second.getSuccessRate();
    }
    
    // Component 3: DV-ETX cost (20%)
    // Lower cost = better, normalize to 0-1 (assume max cost ~ 10)
    float costScore = std::max(0.0f, 1.0f - (cost / 10.0f));
    
    // Component 4: Hop count (10%)
    // Fewer hops = better, normalize (assume max hops ~ 7)
    float hopScore = std::max(0.0f, 1.0f - ((float)hops / 7.0f));
    
    // Weighted composite
    float composite = (linkScore * 0.4f) + (pathScore * 0.3f) + 
                      (costScore * 0.2f) + (hopScore * 0.1f);
    
    return std::max(0.0f, std::min(1.0f, composite));
}
// Purpose: Select alternative path when current path fails
// Inputs: id - packet ID, p - pending packet, failedHop - failed hop (0=unknown)
// Returns: Alternative first hop node, or 0 if no alternative available
// Logic: Filters unhealthy links, unreliable paths, already-attempted hops
// Called by: tryForward() when detecting path failure
NodeNum DtnOverlayModule::selectAlternativePathOnFailure(uint32_t id, Pending &p, NodeNum failedHop)
{
    NodeNum dest = p.data.orig_to;
    
    // Get packet history to avoid retrying same paths
    auto& history = packetPathHistoryMap[id];
    history.packetId = id;
    history.destination = dest;
    
    // Mark failed hop
    if (failedHop != 0 && !history.hasAttempted(failedHop)) {
        history.recordAttempt(failedHop);
    }
    
    // Check if we've exhausted maximum path switches
    if (history.pathSwitchCount >= configMaxPathSwitches) {
        LOG_WARN("DTN AdaptiveReroute: Max path switches reached (%u) for id=0x%x",
                 configMaxPathSwitches, id);
        return 0;
    }
    
    // Discover all possible paths to destination
    std::vector<PathCandidate> candidates;
    
    // Candidate 1: Direct neighbor (if dest is neighbor)
    if (isDirectNeighbor(dest)) {
        PathCandidate direct;
        direct.nextHop = dest;
        direct.hopCount = 1;
        direct.cost = 1.0f;
        direct.score = 1.0f; // Best possible
        candidates.push_back(direct);
    }
    
    // Candidate 2: NextHopRouter paths (DV-ETX routing)
    if (router) {
        if (auto nh = router->asNextHopRouter()) {
            auto snap = nh->getRouteSnapshot(false);
        
        for (const auto &e : snap) {
            if (e.dest == dest) {
                //FIX #142: Guard getMeshNodeByIndex() against out-of-bounds
                // PROBLEM: e.next_hop (DV-ETX index) may be >= numMeshNodes if NodeDB changed
                //          Result: assert failed (x < numMeshNodes) → ESP32 crash!
                // ROOT CAUSE: Race condition - routing snapshot vs NodeDB size
                // SOLUTION: Check bounds BEFORE calling getMeshNodeByIndex()
                //Map DV-ETX next_hop byte to actual NodeNum (routes snapshot stores low byte only)
                NodeNum mapped = resolveNextHopNode(e.next_hop);
                if (mapped == 0) {
                    LOG_WARN("DTN AdaptiveReroute: Unknown next_hop byte %u for dest 0x%x - skipping",
                             e.next_hop, dest);
                    continue;
                }

                if (mapped == failedHop) continue;

                //FIX #144: Never select ourselves as alternative path
                // PROBLEM: AdaptiveReroute can select our own nodeID (0x1f9ffb04) as "alternative"
                //          Result: custody send to=self → enqueued local → never sent over RF → STUCK!
                // SOLUTION: Skip if mapped == our nodeNum
                if (mapped == nodeDB->getNodeNum()) {
                    LOG_WARN("DTN AdaptiveReroute: Skipping self (0x%x) as alternative - would create loop",
                             mapped);
                    continue;
                }
                
                PathCandidate dvEtx;
                dvEtx.nextHop = mapped;
                dvEtx.hopCount = getHopsAway(dest);
                dvEtx.cost = e.aggregated_cost;
                dvEtx.score = calculatePathScore(mapped, dest, dvEtx.cost, dvEtx.hopCount);
                candidates.push_back(dvEtx);
                break; // Found route for this destination
            }
        }
        }
    }
    
    // Candidate 3: Alternative paths via FW+ neighbors
    for (const auto& entry : fwplusVersionByNode) {
        NodeNum neighbor = entry.first;
        
        //FIX #144: Never select ourselves as alternative path (part 2)
        if (neighbor == nodeDB->getNodeNum()) continue;
        
        // Skip if not a direct neighbor
        if (!isDirectNeighbor(neighbor)) continue;
        
        // Skip if same as failed hop or already attempted
        if (neighbor == failedHop || history.hasAttempted(neighbor)) {
            continue;
        }
        
        // Check if this neighbor might know route to destination
        // (We can't directly query their routing table, so use heuristics)
        uint8_t hopsToNbr = 1; // Direct neighbor
        uint8_t hopsFromNbrToDest = getHopsAway(dest); // Our estimate
        
        if (hopsFromNbrToDest != 255) { // We know path from here
            PathCandidate alt;
            alt.nextHop = neighbor;
            alt.hopCount = hopsToNbr + hopsFromNbrToDest;
            alt.cost = 1.0f + hopsFromNbrToDest; // Simple estimate
            alt.score = calculatePathScore(neighbor, dest, alt.cost, alt.hopCount);
            candidates.push_back(alt);
        }
    }
    
    if (candidates.empty()) {
        LOG_WARN("DTN AdaptiveReroute: No alternative paths found for dest 0x%x", dest);
        return 0;
    }
    
    // Filter candidates based on health and reliability
    std::vector<PathCandidate> viableCandidates;
    std::vector<PathCandidate> filteredButBestCandidates; //Track best filtered candidates
    
    for (const auto& candidate : candidates) {
        NodeNum hop = candidate.nextHop;
        
        // Filter 1: Skip already-attempted hops
        if (history.hasAttempted(hop)) {
            LOG_DEBUG("DTN AdaptiveReroute: Skip already-attempted 0x%x", hop);
            continue;
        }
        
        // Filter 2: Skip unhealthy links
        if (!isLinkHealthy(hop)) {
            LOG_DEBUG("DTN AdaptiveReroute: Skip unhealthy link 0x%x", hop);
            filteredButBestCandidates.push_back(candidate); //Keep for fallback
            continue;
        }
        
        // Filter 3: Skip unreliable paths (from historical data)
        if (!isPathReliable(hop, dest)) {
            LOG_DEBUG("DTN AdaptiveReroute: Skip unreliable path via 0x%x", hop);
            filteredButBestCandidates.push_back(candidate); //Keep for fallback
            continue;
        }
        
        viableCandidates.push_back(candidate);
    }
    
    //NEW: Intelligent path unblock when all paths filtered out
    // PROBLEM: All FW+ paths marked unhealthy/unreliable → no alternative → deadlock
    //          Example: Node13→Node1, paths 0x1b, 0x1c all BLOCKED → broadcast storm
    // SOLUTION: When no viable paths, temporarily unblock best filtered candidate
    //           Give it ONE shot to prove it's recovered, otherwise re-block
    // RATIONALE: Networks are dynamic - links recover, blocked paths may work again
    //            Better to retry best historical path than immediate broadcast
    if (viableCandidates.empty() && !filteredButBestCandidates.empty()) {
        LOG_WARN("DTN AdaptiveReroute: All candidate paths filtered out for dest 0x%x", dest);
        
        // Find best filtered candidate (highest score = best historical performance)
        auto bestIt = std::max_element(filteredButBestCandidates.begin(), 
                                      filteredButBestCandidates.end(),
                                      [](const PathCandidate& a, const PathCandidate& b) {
                                          return a.score < b.score;
                                      });
        
        if (bestIt != filteredButBestCandidates.end()) {
            NodeNum bestHop = bestIt->nextHop;
            LOG_WARN("DTN AdaptiveReroute: Temporarily unblocking best filtered path 0x%x (score=%.2f) for one-shot retry",
                    bestHop, bestIt->score);
            
            // Temporarily unblock by adding to viable candidates
            // If this attempt fails, normal path learning will re-block it
            viableCandidates.push_back(*bestIt);
            ctrAdaptiveReroutes++; // Count as adaptive reroute (forced unblock)
        }
    }
    
    if (viableCandidates.empty()) {
        LOG_WARN("DTN AdaptiveReroute: No alternative paths available for dest 0x%x (all filtered, no candidates)", dest);
        return 0; // No viable alternative
    }
    
    // Sort by score (best first)
    std::sort(viableCandidates.begin(), viableCandidates.end(),
              [](const PathCandidate& a, const PathCandidate& b) {
                  return a.score > b.score; // Higher score = better
              });
    
    // Select best viable candidate
    NodeNum altHop = viableCandidates[0].nextHop;
    history.recordAttempt(altHop);
    
    LOG_INFO("DTN AdaptiveReroute: Selected alternative 0x%x for dest 0x%x (score=%.2f hops=%u cost=%.2f)",
             altHop, dest, viableCandidates[0].score, 
             viableCandidates[0].hopCount, viableCandidates[0].cost);
    
    ctrAdaptiveReroutes++;
    return altHop;
}

//==============================================================================
// ADAPTIVE PATH SELECTION & FAULT TOLERANCE - MAINTENANCE
//==============================================================================

// Purpose: Periodic maintenance in runOnce() main loop
// Effect: Cleans up stale data, logs statistics, updates health metrics
// Called: Every 5 minutes from runOnce()
void DtnOverlayModule::runPeriodicMaintenance()
{
    uint32_t now = millis();
    uint32_t maintenanceIntervalMs = 300000; // 5 minutes
    
    if ((now - lastMaintenanceMs) < maintenanceIntervalMs) {
        return; // Not time yet
    }
    
    lastMaintenanceMs = now;
    
    LOG_INFO("DTN: Running periodic maintenance...");
    
    // Maintain link health data
    maintainLinkHealth();
    
    // Maintain path reliability data
    maintainPathReliability();
    
    // Log comprehensive statistics
    logAdaptiveRoutingStatistics();
}
//Process scheduled hello-backs (RF collision prevention)
void DtnOverlayModule::processScheduledHellobacks()
{
    //FIX #176: Don't process hello-backs if DTN disabled
    if (!configEnabled) {
        scheduledHelloBackMs.clear(); // Clear any scheduled hello-backs
        return;
    }
    
    if (scheduledHelloBackMs.empty()) {
        return; // No scheduled hello-backs
    }
    
    uint32_t nowMs = millis();
    std::vector<NodeNum> toSend; // Collect nodes to send (avoid iterator invalidation)
    
    // Find hello-backs ready to send
    for (auto it = scheduledHelloBackMs.begin(); it != scheduledHelloBackMs.end(); ) {
        NodeNum target = it->first;
        uint32_t scheduledTime = it->second;
        
        if (nowMs >= scheduledTime) {
            // Time to send!
            toSend.push_back(target);
            it = scheduledHelloBackMs.erase(it); // Remove from schedule
        } else {
            ++it;
        }
    }
    
    // Send hello-backs
    for (NodeNum target : toSend) {
        // Double-check rate limit (should be OK since we scheduled it correctly)
        auto it = lastHelloBackToNodeMs.find(target);
        if (it != lastHelloBackToNodeMs.end() && (nowMs - it->second) < 10000) {
            LOG_WARN("DTN: Skipping hello-back to 0x%x (rate limit, last tx %u ms ago)",
                     (unsigned)target, nowMs - it->second);
            continue;
        }
        
        // Check channel utilization
        if (airTime && !airTime->isTxAllowedChannelUtil(true)) {
            LOG_DEBUG("DTN: Deferring hello-back to 0x%x (channel busy, rescheduling +5s)",
                     (unsigned)target);
            // Reschedule for 5s later
            scheduledHelloBackMs[target] = nowMs + 5000;
            continue;
        }
        
        // Send hello-back!
        uint32_t reason = (uint32_t)FW_PLUS_VERSION;
        emitReceipt(target, 0, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, reason);
        lastHelloBackToNodeMs[target] = nowMs;
        updateGlobalProbeTimestamp();
        LOG_INFO("DTN: Sent scheduled hello-back to 0x%x (random delay expired)",
                 (unsigned)target);
    }
}
// Purpose: Log detailed statistics about adaptive routing
// Effect: Outputs comprehensive stats to help debug and monitor system
void DtnOverlayModule::logAdaptiveRoutingStatistics()
{
    LOG_INFO("=== DTN ADAPTIVE ROUTING STATISTICS ===");
    
    // Link health summary
    uint32_t healthyLinks = 0;
    uint32_t unhealthyLinks = 0;
    for (const auto& entry : linkHealthMap) {
        if (isLinkHealthy(entry.first)) {
            healthyLinks++;
        } else {
            unhealthyLinks++;
        }
    }
    LOG_INFO("Links: %u total (%u healthy, %u unhealthy)",
             (unsigned)linkHealthMap.size(), healthyLinks, unhealthyLinks);
    
    // Path reliability summary
    uint32_t reliablePaths = 0;
    uint32_t unreliablePaths = 0;
    uint32_t blockedPaths = 0;
    for (const auto& entry : pathReliabilityMap) {
        const auto& rel = entry.second;
        if (rel.temporarilyBlocked) {
            blockedPaths++;
        } else if (rel.isUnreliable()) {
            unreliablePaths++;
        } else {
            reliablePaths++;
        }
    }
    LOG_INFO("Paths: %u total (%u reliable, %u unreliable, %u blocked)",
             (unsigned)pathReliabilityMap.size(), reliablePaths, 
             unreliablePaths, blockedPaths);
    
    // Adaptive rerouting counters
    LOG_INFO("Adaptive reroutes: %u total, Link checks: %u, Path updates: %u",
             ctrAdaptiveReroutes, ctrLinkHealthChecks, ctrPathLearningUpdates);
    
    LOG_INFO("=======================================");
}

// Purpose: Format hop list for logging
// Inputs: hops - vector of node numbers
// Returns: Formatted string like "0x11, 0x12, 0x13"
// Used by: Logging functions to display path history
std::string DtnOverlayModule::formatHopList(const std::vector<NodeNum>& hops) const
{
    if (hops.empty()) return "none";
    
    std::ostringstream oss;
    for (size_t i = 0; i < hops.size(); ++i) {
        oss << "0x" << std::hex << hops[i];
        if (i < hops.size() - 1) oss << ", ";
    }
    return oss.str();
}

#endif // __has_include("mesh/generated/meshtastic/fwplus_dtn.pb.h")
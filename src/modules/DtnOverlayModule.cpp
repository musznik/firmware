/*
//DTN Overlay Module — Overview (DTN-first custody, FW+ handoff)

Purpose
- Opportunistic store–carry–forward for private Direct Messages. With DTN enabled, private TEXT unicasts are intercepted
  at the sender and wrapped as FWPLUS_DTN DATA (DTN-first). Native DM is used as an intelligent fallback for stock
  destinations or late in TTL. Stock nodes require no changes; FW+ nodes unwrap at the destination and inject the original DM locally.

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
- Primary action: send FWPLUS_DTN DATA toward the destination, or hand off custody to a better FW+ neighbor.
- When the destination is far: prefer handoff to a FW+ neighbor (safe mapping of `next_hop` → unique direct neighbor).
- Shortlist up to 3 FW+ neighbors closer to the destination; rotate between them across retries; per‑destination preference cache.
- Optimistic handoff: when route to destination is unknown (255), use closest FW+ neighbors - they may have better topology knowledge.
- Prevent loops via lastCarrier/self/dest checks and reachability validation.
- Broadcast as last resort: aggressive for unknown routes (after 1 retry or 40% TTL), conservative for known (20% TTL tail).
- Broadcast: public (unencrypted), channel‑utilization gated, randomized cooldown 60–120s to allow 7-hop propagation.
- Priority defaults to BACKGROUND; escalate to DEFAULT in TTL tail when near the destination or when we are the source.
- Global active cap and per‑destination spacing limit concurrency.

Receipts & Milestones
- Any receipt (DELIVERED/PROGRESSED/EXPIRED) clears pending and sets a tombstone to avoid re‑enqueue storms.
- PROGRESSED is emitted sparingly (ring‑gated, auto‑limited) with hysteresis under high load.
- Version advertisement: 32‑bit reason field carries FW+ version number; receiver extracts lower 16 bits.

Traceroute & Fallback
- Source: immediate traceroute hint on low confidence (cooldown), no periodic active probing.
- Stock destination: early native DM fallback; DTN broadcast only in TTL tail (public, cooldown, load gating).
- Intermediates: anti‑burst and far‑throttle; coordination via suppression after hearing foreign DATA.
- Cold start: intermediate nodes use native DM fallback for unknown destinations during cold start (aggressive discovery triggered).
- Unresponsive FW+: tracks consecutive failed DTN attempts to FW+ destinations; after N failures + timeout without receipt,
  triggers native DM fallback. Handles version incompatibility, offline nodes, disabled DTN, and implementation changes.
  Counter resets on receipt. Stock marking cleared when node sends beacon/OnDemand (DTN re-enabled scenario).

Version Advertisement & Discovery
- One‑shot public beacon after start (aggressive, retried on failure), followed by staged periodic beacons.
- Staged warmup: first hour = 4 beacons @ 15min (aggressive), then 2h if unknown, finally 6h when FW+ nodes known.
- Hello-back: unicast version up to 3 hops (rate‑limited, more frequent for known FW+ nodes).
- Passive discovery: OnDemand responses, overheard DTN DATA/RECEIPTs; no periodic active probing.
- Aggressive discovery: triggered during cold start; probes all direct neighbors for FW+ capability.
- Dynamic re-evaluation: newly discovered DTN nodes trigger rescheduling of pending messages for potential handoff.
- Intelligent interception: local DMs only intercepted when DTN nodes are known AND can help reach destination (proximity/topology).

Mobility Adaptation
- Route invalidation: periodically invalidates stale routes for mobile nodes based on last_heard time and mobility factor.
- Adaptive timeouts: shorter timeouts for mobile nodes (15-30min) vs stationary nodes (2h).
- Mobility-aware scheduling: reduced spacing (60% when mobile), earlier attempts, and adjusted backoff for mobile nodes.
- Dynamic parameters: retry backoff, spacing, and max tries adapt to mobility factor in real-time.

Airtime protections
- Channel utilization gate; deterministic election and mobility‑aware slotting; per‑destination spacing; global active cap;
  suppression after hearing foreign overlay DATA; neighbor/grace heuristics; tombstones and bounded caches with pruning.
- Near-destination extra suppression (2-3× longer) to prevent duplicate deliveries close to destination.

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

// Purpose: unwrap received DTN payload for local delivery (destination == us).
// Behavior: injects original DM to local Router as encrypted or decoded variant, preserving original sender id.
void DtnOverlayModule::deliverLocal(const meshtastic_FwplusDtnData &d)
{
    if (d.is_encrypted) {
        meshtastic_MeshPacket *p = allocDataPacket();
        // Preserve original DM id for deduplication and proper UX threading
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(p->encrypted.bytes, d.payload.bytes, d.payload.size);
        p->encrypted.size = d.payload.size;
        p->channel = d.channel;
        // Do not CC to phone here; handleFromRadio will forward once. CC would cause double UI delivery.
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    } else {
        meshtastic_MeshPacket *p = allocDataPacket();
        // Preserve original DM id for deduplication and proper UX threading
        p->id = d.orig_id;
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->decoded.payload.size = (d.payload.size > sizeof(p->decoded.payload.bytes)) ? sizeof(p->decoded.payload.bytes) : d.payload.size;
        memcpy(p->decoded.payload.bytes, d.payload.bytes, p->decoded.payload.size);
        p->channel = d.channel;
        // Do not CC to phone here; handleFromRadio will forward once. CC would cause double UI delivery.
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
}

// Purpose: export a consistent read-only snapshot of DTN runtime counters and flags.
void DtnOverlayModule::getStatsSnapshot(DtnStatsSnapshot &out) const
{
    out.pendingCount = pendingById.size();
    out.forwardsAttempted = ctrForwardsAttempted;
    out.fallbacksAttempted = ctrFallbacksAttempted;
    out.receiptsEmitted = ctrReceiptsEmitted;
    out.receiptsReceived = ctrReceiptsReceived;
    out.expired = ctrExpired;
    out.giveUps = ctrGiveUps;
    out.milestonesSent = ctrMilestonesSent;
    out.probesSent = ctrProbesSent;
    out.fwplusUnresponsiveFallbacks = ctrFwplusUnresponsiveFallbacks;
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
    
    // Adaptive routing statistics
    out.adaptiveReroutes = ctrAdaptiveReroutes;
    out.linkHealthChecks = ctrLinkHealthChecks;
    out.pathLearningUpdates = ctrPathLearningUpdates;
    out.monitoredLinks = linkHealthMap.size();
    out.monitoredPaths = pathReliabilityMap.size();
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
#ifdef ARCH_PORTDUINO
    // For simulator/testing: enable DTN by default on native builds
    configEnabled = true; // module master switch
#else
    configEnabled = false; // module master switch (default OFF); enable via ModuleConfig
#endif
    configTtlMinutes = 4; // DTN custody TTL [minutes] for overlay items; shorter window to limit carry
    configInitialDelayBaseMs = 2000; //reduced base delay before first attempt [ms] for faster delivery
    //soften: larger retry backoff to reduce overlay traffic rate
    configRetryBackoffMs = 120000; // retry backoff between attempts [ms]
    configMaxTries = 2; // max overlay forward attempts per item
    configLateFallback = false; // enable late native-DM fallback near TTL tail
    configFallbackTailPercent = 20; // start fallback in the last X% of TTL [%]
    configMilestonesEnabled = false; // emit sparse PROGRESSED milestones (telemetry); default OFF
    //soften: larger per-destination spacing to avoid bursts
    configPerDestMinSpacingMs = 60000; // per-destination minimum spacing between attempts [ms] (1 min, max uint16_t=65535)
    configMaxActiveDm = 1; // global cap of active DTN attempts per scheduler pass
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
    
    moduleStartMs = millis(); //fw+
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
    //   - Warmup beacons @ 15min intervals (4 total in 1h)
    //   - Post-warmup @ 2h if no FW+ discovered
    //   - Normal @ 6h when FW+ nodes known
    
    //periodically advertise our FW+ version for passive discovery
    maybeAdvertiseFwplusVersion();
    //periodically invalidate stale routes for mobile nodes
    invalidateStaleRoutes();
    //detailed logging and monitoring
    logDetailedStats();
    //simple scheduler: attempt forwards whose time arrived
    uint64_t nowEpoch = getEpochMs(); // helper: safe epoch ms with overflow protection
    uint32_t dmIssuedThisPass = 0; // reset per scheduler pass
    //dynamic wake: compute nearest nextAttempt across pendings
    uint32_t nextWakeMs = 2000;
    for (auto it = pendingById.begin(); it != pendingById.end();) {
        uint32_t currentId = it->first; // Save ID before tryForward() might invalidate iterator
        Pending &p = it->second;
        
        // CRITICAL: Check deadline BEFORE tryForward() to avoid use-after-free
        // If tryForward() erases the entry, we must not access p after that
        if (p.data.deadline_ms && nowEpoch > 0 && nowEpoch > p.data.deadline_ms) {
            // emit EXPIRED receipt to source and drop
            LOG_WARN("DTN expire id=0x%x dl=%u now=%u", currentId, (unsigned)p.data.deadline_ms, (unsigned)nowEpoch);
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
                // CRITICAL: Save size before tryForward() - it might erase the entry
                size_t sizeBefore = pendingById.size();
                tryForward(currentId, p);
                dmIssuedThisPass++;
                
                // CRITICAL: Check if tryForward() erased the entry (iterator invalidation protection)
                if (pendingById.size() < sizeBefore) {
                    // Entry was removed by tryForward() - iterator is invalidated
                    // We must restart iteration to be safe
                    it = pendingById.find(currentId);
                    if (it == pendingById.end()) {
                        // Entry was indeed removed - get next entry
                        it = pendingById.begin();
                        // Find first entry after currentId to avoid re-processing
                        while (it != pendingById.end() && it->first <= currentId) {
                            ++it;
                        }
                        continue;
                    }
                    // Entry still exists (weird but possible) - fall through to ++it
                }
            } else {
                // push a bit forward
                p.nextAttemptMs = now + 1000 + (uint32_t)random(500);
                LOG_DEBUG("DTN defer(id=0x%x): glob cap reached, next in %u ms", currentId, (unsigned)(p.nextAttemptMs - now));
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

// Purpose: check if node is "alive" (seen recently via last_heard)
// Returns: true if node was seen within configMaxNodeAgeSec (24h default)
bool DtnOverlayModule::isNodeAlive(NodeNum node) const
{
    meshtastic_NodeInfoLite *nodeInfo = nodeDB->getMeshNode(node);
    if (!nodeInfo) return false;
    
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
    uint32_t lastSeenEpoch = nodeInfo->last_heard;
    
    if (nowEpoch == 0 || lastSeenEpoch == 0) return false;
    return (nowEpoch - lastSeenEpoch) <= configMaxNodeAgeSec;
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
    
    // CRITICAL: Global milestone rate limit to prevent burst (watchdog protection)
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
    
    // Tombstone to avoid re-emitting on repeated hears
    if (configTombstoneMs) {
        tombstoneUntilMs[data.orig_id] = millis() + configTombstoneMs;
    }
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
    
    // Use configured TTL instead of fixed 5 minutes
    uint32_t ttlMinutes = (configTtlMinutes ? configTtlMinutes : 5);
    uint64_t deadline = getEpochMs() + (ttlMinutes * 60ULL * 1000ULL);
    
    if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
        // Policy: by default do NOT capture foreign encrypted unicasts to avoid DTN-on-DTN in mixed meshes
        if (!configCaptureForeignEncrypted) return false;
        
        // Tombstone check: avoid rapid re-enqueue of same orig_id
        auto itTs = tombstoneUntilMs.find(mp.id);
        if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) return false;
        
        enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                            deadline,
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
                            deadline,
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
    
    // CRITICAL: Global rate limiter check
    if (isGlobalProbeCooldownActive()) {
        LOG_DEBUG("DTN: Telemetry probe blocked by global rate limiter (need %u sec)", 
                 getGlobalProbeCooldownRemainingSec());
        return;
    }
    
    // CRITICAL: Only probe LIVE nodes (seen within 24h)
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
        
        // CRITICAL: If DTN is disabled, reject intermediate custody but ALWAYS deliver if we're destination
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
        
        // DTN enabled - mark sender as FW+ capable and process normally
        markFwplusSeen(getFrom(&mp));
        
        LOG_INFO("DTN rx DATA id=0x%x from=0x%x to=0x%x (mp.to=0x%x) enc=%d dl=%u", msg->variant.data.orig_id,
                 (unsigned)getFrom(&mp), (unsigned)msg->variant.data.orig_to, (unsigned)mp.to,
                 (int)msg->variant.data.is_encrypted, (unsigned)msg->variant.data.deadline_ms);
        
        if (isBroadcast || isForUs || dataDestIsUs) {
            // Process milestone emission (sparse PROGRESSED receipts)
            processMilestoneEmission(mp, msg->variant.data);
            
            // Handle the DATA (will check internally if dest == us for delivery)
            handleData(mp, msg->variant.data);
            
            // Consume broadcasts and packets for us
            if (isBroadcast) {
                return false; // Broadcast - don't consume, let it propagate
            } else {
                return true; // Unicast for us - consume
            }
        } else {
            // DATA packet for someone else (mp.to != us) - let Router relay it
            LOG_DEBUG("DTN rx DATA (relay - not for us, mp.to=0x%x)", (unsigned)mp.to);
            return false; // Don't consume - Router will relay
        }
    }
    
    // Handle DTN RECEIPT packets
    if (msg && msg->which_variant == meshtastic_FwplusDtn_receipt_tag) {
        // Check if this packet is FOR US or is a broadcast (beacon)
        bool isForUs = (mp.to == nodeDB->getNodeNum());
        bool isBroadcast = (mp.to == NODENUM_BROADCAST || mp.to == NODENUM_BROADCAST_NO_LORA);
        
        // CRITICAL: If DTN is disabled, we still observe other FW+ nodes (passive discovery)
        // but we DON'T advertise ourselves as FW+ capable (skip markFwplusSeen for self)
        // We DO process RECEIPTs for us (delivery confirmations) even if disabled
        if (!configEnabled) {
            if (isForUs) {
                // RECEIPT for us - process even if DTN disabled (e.g., reply to probe we sent before disabling)
                LOG_INFO("DTN rx RECEIPT id=0x%x status=%u from=0x%x (FOR US, DTN disabled)", 
                         msg->variant.receipt.orig_id, (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp));
                // Observe sender's FW+ capability (passive learning)
                if (msg->variant.receipt.reason > 0) {
                    NodeNum origin = getFrom(&mp);
                    uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                    recordFwplusVersion(origin, ver);
                }
                // Don't call handleReceipt() - we have no pending DTN packets when disabled
                return true; // Consume - this is for us
            } else {
                // Broadcast or transit RECEIPT - observe but don't mark ourselves as FW+
                if (isBroadcast || msg->variant.receipt.reason > 0) {
                    NodeNum origin = getFrom(&mp);
                    uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                    if (ver > 0) {
                        LOG_DEBUG("DTN: Passive discovery - node 0x%x is FW+ v%u (DTN disabled)", 
                                 (unsigned)origin, (unsigned)ver);
                        recordFwplusVersion(origin, ver);
                    }
                }
                return false; // Don't consume - let it propagate/relay
            }
        }
        
        // DTN enabled - mark sender as FW+ capable and process normally
        markFwplusSeen(getFrom(&mp));
        
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
            return true; // Consume - this is for us
        } else {
            // Unicast RECEIPT for someone else - observe for discovery but DON'T consume (let Router relay)
            LOG_DEBUG("DTN rx RECEIPT id=0x%x from=0x%x to=0x%x (relay - not for us)", 
                     msg->variant.receipt.orig_id, (unsigned)getFrom(&mp), (unsigned)mp.to);
            // Still process for discovery (already called markFwplusSeen above)
            // But check for version advertisement
            if (msg->variant.receipt.reason > 0) {
                LOG_DEBUG("DTN: Observing version advertisement in transit packet (reason=0x%x)", (unsigned)msg->variant.receipt.reason);
                // We can still learn about FW+ nodes from packets we relay
                NodeNum origin = getFrom(&mp);
                uint16_t ver = (uint16_t)(msg->variant.receipt.reason & 0xFFFFu);
                recordFwplusVersion(origin, ver);
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

// Purpose: create DTN envelope from a captured DM (plaintext or ciphertext) and schedule forwarding.
bool DtnOverlayModule::enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                                           uint64_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                                           bool allowProxyFallback)
{
    // CRITICAL: Check tombstone first to prevent re-capture during retransmissions
    auto itTs = tombstoneUntilMs.find(origId);
    if (itTs != tombstoneUntilMs.end() && millis() < itTs->second) {
        LOG_DEBUG("DTN: Skip re-capture of id=0x%x - tombstone active (prevents retrans loop)", origId);
        return false; // Packet skipped (tombstone active)
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
    d.orig_rx_time = getValidTime(RTCQualityFromNet);
    //Safe cast: clamp deadline to uint32 max (proto limitation until we migrate to uint64)
    d.deadline_ms = (deadlineMs > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (uint32_t)deadlineMs;
    d.is_encrypted = isEncrypted;
    d.allow_proxy_fallback = allowProxyFallback;
    memcpy(d.payload.bytes, bytes, size);
    d.payload.size = size;
    
    // CRITICAL: Create tombstone to prevent re-capture during Router retransmissions
    // Without this, Router retries will be re-captured by DTN creating infinite loop
    // Tombstone duration = TTL to prevent any re-capture until packet expires
    if (configTombstoneMs && deadlineMs > 0) {
        uint64_t nowMs = getEpochMs();
        uint32_t tombstoneDuration;
        if (nowMs > 0 && deadlineMs > nowMs) {
            // Calculate remaining TTL
            uint64_t ttlRemaining = deadlineMs - nowMs;
            tombstoneDuration = (ttlRemaining > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (uint32_t)ttlRemaining;
        } else {
            // Fallback: use configTtlMinutes
            tombstoneDuration = (configTtlMinutes * 60UL * 1000UL);
        }
        tombstoneUntilMs[origId] = millis() + tombstoneDuration;
        LOG_DEBUG("DTN: Created tombstone for id=0x%x (duration=%u ms) to prevent re-capture", 
                 origId, tombstoneDuration);
    }
    
    //Calculate TTL for logging (deadline - now)
    uint64_t nowMs = getEpochMs(); // helper: safe epoch ms
    uint64_t ttlMs64 = (deadlineMs > nowMs) ? (deadlineMs - nowMs) : 0;
    uint32_t ttlMs = (ttlMs64 > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (uint32_t)ttlMs64;
    LOG_DEBUG("DTN capture id=0x%x src=0x%x dst=0x%x enc=%d ch=%u ttlms=%u", origId, (unsigned)origFrom,
              (unsigned)origTo, (int)isEncrypted, (unsigned)channel, ttlMs);
    scheduleOrUpdate(origId, d);
    
    return true; // Packet was successfully enqueued
}

// Purpose: process received FWPLUS_DTN DATA; deliver locally if destined to us, else schedule and coordinate with peers.
void DtnOverlayModule::handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d)
{
    // If we are the destination, deliver locally
    if (d.orig_to == nodeDB->getNodeNum()) {
        //Race condition protection: check-and-set tombstone BEFORE delivery
        // This prevents duplicate delivery if two packets arrive nearly simultaneously
        auto itDelivered = tombstoneUntilMs.find(d.orig_id);
        if (itDelivered != tombstoneUntilMs.end() && millis() < itDelivered->second) {
            LOG_DEBUG("DTN: Ignoring duplicate delivery for id=0x%x (already delivered)", d.orig_id);
            ctrDuplicatesSuppressed++; //metric
            return; // Already delivered, ignore duplicate
        }
        
        //Set tombstone BEFORE delivery (atomic-like check-and-set)
        if (configTombstoneMs) {
            tombstoneUntilMs[d.orig_id] = millis() + configTombstoneMs;
        }
        
        // Now safe to deliver (tombstone set, future duplicates will be rejected)
        deliverLocal(d);
        ctrDeliveredLocal++; //metric
        
        // Send DELIVERED receipt back to source (include via=proxy in reason low byte)
        uint32_t via = nodeDB->getNodeNum() & 0xFFu; // 1-byte hint
        LOG_INFO("DTN delivered id=0x%x to=0x%x src=0x%x via=0x%x", d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from, (unsigned)via);
        emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED, via);
        
        //Send native ACK to ALL senders (FW+ and stock) for proper app UX
        // Ensures apps show "delivered" instead of "pending"
        routingModule->sendAckNak(meshtastic_Routing_Error_NONE, d.orig_from, d.orig_id, d.channel, 0);
        
        //Clear stock marking if we successfully received DTN delivery (proves we are FW+)
        // This is our own node, so clear our nodeNum from stock list if it was mistakenly added
        auto itStock = stockKnownMs.find(nodeDB->getNodeNum());
        if (itStock != stockKnownMs.end()) {
            LOG_DEBUG("DTN: Clearing our own stock marking (we just received DTN!)");
            stockKnownMs.erase(itStock);
        }
        
        // Anti-storm: immediately silence local pending for this ID
        // Prevents continued attempts after successful delivery
        pendingById.erase(d.orig_id);
        
        return;
    }
    // Otherwise, coordinate with others: if we heard someone else carrying this id, suppress our attempt for a while 
    auto it = pendingById.find(d.orig_id);
    if (it == pendingById.end()) {
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
        
        // First time we see this id (from overlay or capture) → schedule normally
        scheduleOrUpdate(d.orig_id, d);
        // Track who carried this packet for loop detection
        auto &p = pendingById[d.orig_id];
        trackCarrier(p, getFrom(&mp));
        
        // CRITICAL FIX: Only apply suppression if we overheard the DATA (not unicast to us)
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
        
        // CRITICAL FIX: Only apply suppression if we overheard (not unicast to us)
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
    
    //Reset unresponsive tracking before erasing (destination is responsive!)
    auto itPending = pendingById.find(r.orig_id);
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
            // We know which path(s) we tried
            bool success = (r.status == meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED);
            
            // Update path reliability for all attempted hops
            for (NodeNum hop : history.attemptedHops) {
                updatePathReliability(hop, history.destination, success);
                
                // Update link health for the first hop we tried (most recent)
                if (hop == history.attemptedHops.back()) {
                    // Estimate signal quality (use last known if available)
                    auto healthIt = linkHealthMap.find(hop);
                    float snr = (healthIt != linkHealthMap.end()) ? healthIt->second.avgSnr : 5.0f;
                    float rssi = (healthIt != linkHealthMap.end()) ? healthIt->second.avgRssi : -80.0f;
                    
                    updateLinkHealth(hop, success, snr, rssi);
                }
            }
            
            if (success) {
                LOG_INFO("DTN PathLearn: Packet id=0x%x SUCCESS via path(s): %s",
                         r.orig_id, formatHopList(history.attemptedHops).c_str());
            } else {
                LOG_WARN("DTN PathLearn: Packet id=0x%x FAILED via path(s): %s (status=%u)",
                         r.orig_id, formatHopList(history.attemptedHops).c_str(), r.status);
            }
            
            // Clean up history for completed packet
            packetPathHistoryMap.erase(r.orig_id);
        }
    }
    
    pendingById.erase(r.orig_id);
    // Create a short tombstone to prevent immediate re-capture storms of same id 
    if (configTombstoneMs) tombstoneUntilMs[r.orig_id] = millis() + configTombstoneMs;
    ctrReceiptsReceived++; //fw+

    //interpret special FW+ version advertisements carried via receipt.reason
    // Convention: reason field contains version number directly (16-bit)
    LOG_DEBUG("DTN: Processing receipt reason=0x%x status=%u", (unsigned)r.reason, (unsigned)r.status);
    if (r.reason > 0) {
        LOG_INFO("DTN: Version advertisement detected, reason=0x%x", (unsigned)r.reason);
        NodeNum origin = getFrom(&mp);
        bool hadVer = (fwplusVersionByNode.find(origin) != fwplusVersionByNode.end());
        uint16_t ver = (uint16_t)(r.reason & 0xFFFFu);
        recordFwplusVersion(origin, ver);

        //fw+ TEST: Immediate version response for faster discovery
        if (!hadVer && ver > 0) {
            LOG_INFO("DTN: New FW+ node 0x%x discovered with version %u - checking if response needed",
                     (unsigned)origin, (unsigned)ver);

            // Send immediate hello-back response with our version
            if (configHelloBackEnabled) {
                // CRITICAL: Global rate limiter check
                if (isGlobalProbeCooldownActive()) {
                    LOG_DEBUG("DTN: Hello-back blocked by global rate limiter (need %u sec)",
                             getGlobalProbeCooldownRemainingSec());
                } else {
                    uint32_t nowMs = millis();
                    auto it = lastHelloBackToNodeMs.find(origin);
                    if (it == lastHelloBackToNodeMs.end() || (nowMs - it->second) >= 10000) { // 10s cooldown for new discoveries
                        if (!(airTime && !airTime->isTxAllowedChannelUtil(true))) {
                            uint32_t reason = (uint32_t)FW_PLUS_VERSION;
                            emitReceipt(origin, 0, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, reason);
                            lastHelloBackToNodeMs[origin] = nowMs;
                            updateGlobalProbeTimestamp();
                            LOG_INFO("DTN: Immediate hello-back sent to new FW+ node 0x%x", (unsigned)origin);
                        }
                    }
                }
            }
        }
        // Optional hello-back: unicast our version to origin (allow periodic responses for FW+DTN discovery)
        LOG_DEBUG("DTN: Version advertisement from origin=0x%x ver=%u hadVer=%d", (unsigned)origin, (unsigned)ver, (int)hadVer);
        if (configHelloBackEnabled) {
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
                    // CRITICAL: Global rate limiter check
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
                            LOG_INFO("DTN: Hello-back sent to origin=0x%x", (unsigned)origin);
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

// Purpose: add carrier to loop detection circular buffer
void DtnOverlayModule::trackCarrier(Pending &p, NodeNum carrier)
{
    p.recentCarriers[p.recentCarrierIndex] = carrier;
    p.recentCarrierIndex = (p.recentCarrierIndex + 1) % 3;
    p.lastCarrier = carrier; // backward compat
}

// Purpose: get current epoch time in milliseconds (uint64 to avoid overflow)
uint64_t DtnOverlayModule::getEpochMs() const
{
    return (uint64_t)getValidTime(RTCQualityFromNet) * 1000ULL;
}

// Purpose: calculate TTL from deadline and orig_rx_time (with overflow protection)
uint64_t DtnOverlayModule::calculateTtl(const meshtastic_FwplusDtnData &d) const
{
    if (d.deadline_ms == 0 || d.orig_rx_time == 0) return 0;
    uint64_t origRxMs = (uint64_t)d.orig_rx_time * 1000ULL;
    return (d.deadline_ms > origRxMs) ? ((uint64_t)d.deadline_ms - origRxMs) : 0;
}

// Purpose: calculate TTL tail start time in epoch milliseconds
uint64_t DtnOverlayModule::calculateTailStart(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const
{
    uint64_t ttl = calculateTtl(d);
    if (ttl == 0) return 0;
    uint32_t percent = (tailPercent > 100) ? 100 : tailPercent;
    return (uint64_t)d.deadline_ms - (ttl * percent / 100);
}

// Purpose: check if we're in TTL tail (returns false if no valid time)
bool DtnOverlayModule::isInTtlTail(const meshtastic_FwplusDtnData &d, uint32_t tailPercent) const
{
    uint64_t nowEpoch = getEpochMs();
    if (nowEpoch == 0 || d.deadline_ms == 0 || d.orig_rx_time == 0) return false;
    uint64_t tailStart = calculateTailStart(d, tailPercent);
    return (nowEpoch >= tailStart);
}

// Purpose: create or refresh a pending DTN entry and compute the next attempt time.
// Inputs: original message id and payload envelope; uses topology, mobility and per-destination spacing.
// Effect: updates election timing, applies far-node throttle, and stores per-dest last TX timestamp.
void DtnOverlayModule::scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d)
{
    auto &p = pendingById[id];
    p.data = d;
    trackCarrier(p, nodeDB->getNodeNum()); // track ourselves as carrier
    
    // Calculate scheduling components using helper functions
    uint32_t base = calculateBaseDelay(d, p);
    uint32_t topologyDelay = calculateTopologyDelay(d);
    uint32_t mobilitySlot = calculateMobilitySlot(id, d, p);
    
    // CRITICAL: For source node's first attempt, add initial delay if no routing confidence
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
    // Get current time for deadline checks
    uint64_t nowEpoch = getEpochMs(); // helper: safe epoch ms
    
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) { 
        p.nextAttemptMs = millis() + 2500 + (uint32_t)random(500); 
        LOG_DEBUG("DTN busy: defer id=0x%x", id); 
        return; 
    }

    // Check max tries limit (always respect deadline even if maxTries=0)
    // Only check deadline if we have valid time
    bool exceedsMaxTries = (configMaxTries > 0 && p.tries >= configMaxTries);
    bool pastDeadline = (p.data.deadline_ms > 0 && nowEpoch > 0 && nowEpoch > p.data.deadline_ms);
    
    if (exceedsMaxTries || pastDeadline) {
        const char* reason = exceedsMaxTries ? "max tries" : "deadline";
        emitReceipt(p.data.orig_from, id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0); 
        LOG_WARN("DTN give up id=0x%x tries=%u reason=%s", id, (unsigned)p.tries, reason);
        pendingById.erase(id);
        ctrGiveUps++;
        return;
    }

    // Check DV-ETX route confidence gating
    bool lowConf = !hasSufficientRouteConfidence(p.data.orig_to);
    bool isFromSource = (p.data.orig_from == nodeDB->getNodeNum());
    
    // CRITICAL: For source node without routing confidence, trigger traceroute for route discovery
    // Traceroute builds routing confidence (1 ACK = confidence 1 = NextHop ready)
    // This helps DTN choose correct handoff direction in subsequent attempts
    if (lowConf && isFromSource && p.tries == 0) {
        // First attempt: trigger traceroute to build routing table
        maybeTriggerTraceroute(p.data.orig_to);
        LOG_INFO("DTN: Source without routing confidence - triggered traceroute to 0x%x (try=%u)",
                 (unsigned)p.data.orig_to, (unsigned)p.tries);
    }
    
    // CRITICAL: No routing confidence fallback for source node
    // DTN custody requires routing hints to avoid wrong-direction handoff.
    // Without routing confidence, we CANNOT reliably choose handoff target.
    // Strategy: Use native DM fallback for reliability (mesh routing handles it).
    // Note: We do NOT assume routing will "warm up" - in large/sparse meshes (200 nodes, 300km)
    //       ACKs may never return, and routing may remain empty for hours or days.
    if (lowConf && isFromSource && p.data.allow_proxy_fallback) {
        LOG_WARN("DTN: No routing confidence to dest 0x%x - using native DM fallback (uptime=%us)",
                 (unsigned)p.data.orig_to, (unsigned)((millis() - moduleStartMs) / 1000));
        if (sendProxyFallback(id, p)) {
            ctrFallbacksAttempted++;
            return; // Fallback sent successfully
        }
    }
    
    //Intermediate nodes with low confidence: defer to allow route discovery
    if (lowConf && !isDirectNeighbor(p.data.orig_to) && !isFromSource) {
        float mobility = fwplus_getMobilityFactor01();
        uint32_t backoff = configRetryBackoffMs + (uint32_t)random(2000);
        if (mobility > 0.5f) backoff = backoff / 2; // try sooner if we are moving
        p.nextAttemptMs = millis() + backoff;
        LOG_DEBUG("DTN: Intermediate low conf defer id=0x%x, next in %u ms", id, backoff);
        return;
    }

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
    
    // Only trigger traceroute for intermediate nodes, not source (source already triggered above)
    if (lowConf && target == p.data.orig_to && !isDirectNeighbor(p.data.orig_to) && !isFromSource) {
        maybeTriggerTraceroute(p.data.orig_to);
        return; // give time for route discovery before attempting
    }

    // Send DTN overlay packet
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_data_tag;
    msg.variant.data = p.data;

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
    
    setPriorityForTailAndSource(mp, p, isFromSource);

    // Check if we should back off (favor closer relayers)
    // CRITICAL: This check is for OVERHEARD packets (foreign capture), not for custody transfers!
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
            p.nextAttemptMs = millis() + configRetryBackoffMs + 5000 + (uint32_t)random(2000);
            LOG_DEBUG("DTN: Deferring - dest further than src (hops: dst=%u src=%u)", 
                     (unsigned)hopsToDest, (unsigned)hopsToSrc);
            return;
        }
        */
    }

    // Send the packet
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    ctrForwardsAttempted++; 
    lastForwardMs = millis(); 
    
    // Update tracking
    lastDestTxMs[p.data.orig_to] = millis();
    if (target != p.data.orig_to) {
        auto &ph = preferredHandoffByDest[p.data.orig_to];
        ph.node = target;
        ph.lastUsedMs = millis();
        ctrHandoffsAttempted++; //metric: custody handoff to another FW+ node
    }
    
    //Track DTN attempt for unresponsive detection (only for direct-to-dest attempts, not handoffs)
    if (target == p.data.orig_to && isFwplus(p.data.orig_to)) {
        p.lastDtnAttemptMs = millis();
        p.dtnFailedAttempts++; // Will be reset if we get a receipt
        LOG_DEBUG("DTN: Tracking attempt %u to FW+ dest 0x%x (unresponsive detection)", 
                 (unsigned)p.dtnFailedAttempts, (unsigned)p.data.orig_to);
    }
    
    // Schedule next attempt
    p.nextAttemptMs = millis() + (configRetryBackoffMs ? configRetryBackoffMs : 60000);
    LOG_INFO("DTN fwd overlay id=0x%x dst=0x%x try=%u next=%u ms", id, (unsigned)p.data.orig_to, (unsigned)p.tries,
             (unsigned)(p.nextAttemptMs - millis()));
}

// Purpose: perform late native DM fallback for a DTN item (encrypted), preserving original id for ACK mapping.
// Returns: true if a send was attempted or queued; schedules next retry on success or alloc failure.
bool DtnOverlayModule::sendProxyFallback(uint32_t id, Pending &p)
{
    // Send native DM (ciphertext or plaintext) towards destination
    meshtastic_MeshPacket *dm = allocDataPacket();
    if (!dm) { p.nextAttemptMs = millis() + 3000; return true; }
    dm->to = p.data.orig_to;
    dm->channel = p.data.channel;
    
    // CRITICAL: Sender spoofing logic
    // For INTERMEDIATE node fallback: spoof sender (from=orig_from) for stock node decryption
    // For SOURCE node fallback: DON'T spoof (from will be set by Router to ourNodeNum)
    //   - Source doesn't need spoofing (we ARE the original sender)
    //   - Spoofing causes re-intercept loop (isDtnFallback check fails)
    bool isSourceFallback = (p.data.orig_from == nodeDB->getNodeNum());
    if (!isSourceFallback) {
        // Intermediate node: spoof sender for proper decryption
        dm->from = p.data.orig_from;
    }
    // else: Source node, leave from=0, Router will set it to ourNodeNum
    if (p.data.is_encrypted) {
        dm->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(dm->encrypted.bytes, p.data.payload.bytes, p.data.payload.size);
        dm->encrypted.size = p.data.payload.size;
    } else {
        dm->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        if (p.data.payload.size > sizeof(dm->decoded.payload.bytes))
            dm->decoded.payload.size = sizeof(dm->decoded.payload.bytes);
        else
            dm->decoded.payload.size = p.data.payload.size;
        memcpy(dm->decoded.payload.bytes, p.data.payload.bytes, dm->decoded.payload.size);
        dm->decoded.want_response = false;
    }
    
    // CRITICAL: ID handling for re-intercept prevention
    // For SOURCE node fallback: use DIFFERENT ID to trigger tombstone check in Router intercept
    //   - Tombstone exists for orig_id (from initial capture)
    //   - Router::send() will call enqueueFromCaptured() which checks tombstone
    //   - Fallback packet will be skipped (tombstone active) → no re-intercept loop!
    // For INTERMEDIATE node: preserve original ID (ACK mapping + spoofed sender detection)
    if (isSourceFallback) {
        dm->id = p.data.orig_id; // Use original ID, tombstone will prevent re-capture
    } else {
        dm->id = p.data.orig_id; // Preserve original ID for ACK mapping
    }
    // CRITICAL: want_ack=false to avoid ReliableRouter tracking conflict with spoofed sender
    // Spoofed sender (from=orig_from) is needed for stock node decryption, but ReliableRouter
    // uses (from, id) as key in pending map, which would conflict with our node's packets.
    // Mesh routing will use flooding which provides natural retries without need for ACK.
    dm->want_ack = false;
    dm->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    
    service->sendToMesh(dm, RX_SRC_LOCAL, false);
    
    p.tries++;
    p.nextAttemptMs = millis() + configRetryBackoffMs;
    
    if (isSourceFallback) {
        LOG_INFO("DTN fallback DM id=0x%x dst=0x%x try=%u (source fallback, no sender spoofing, want_ack=false)", 
                 id, (unsigned)p.data.orig_to, (unsigned)p.tries);
    } else {
        LOG_INFO("DTN fallback DM id=0x%x dst=0x%x try=%u (intermediate fallback, spoofed from=0x%x, want_ack=false)", 
                 id, (unsigned)p.data.orig_to, (unsigned)p.tries, (unsigned)p.data.orig_from);
    }
    ctrFallbacksAttempted++; 
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
    
    // CRITICAL: Global rate limiter check
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
    p->want_ack = false; // CRITICAL: No radio ACK! We want the traceroute REPLY, not hop-by-hop ACKs
    
    meshtastic_RouteDiscovery req = meshtastic_RouteDiscovery_init_zero;
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), 
                                                  &meshtastic_RouteDiscovery_msg, &req);
    
    // Set hop_limit based on known distance (expanding ring)
    // CRITICAL: Response needs same hop_limit to return, so double the distance + margin
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
void DtnOverlayModule::emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason)
{
    // Guard: don't emit receipts if DTN is disabled (don't advertise as FW+ capable)
    if (!configEnabled) return;
    
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_receipt_tag;
    msg.variant.receipt.orig_id = origId;
    msg.variant.receipt.status = status;
    msg.variant.receipt.reason = reason;

    meshtastic_MeshPacket *p = allocDataProtobuf(msg);
    if (!p) return;
    p->to = to;
    // Ensure receipt routes back normally
    p->from = nodeDB->getNodeNum();
    p->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_DEBUG("DTN tx RECEIPT id=0x%x status=%u to=0x%x", (unsigned)origId, (unsigned)status, (unsigned)to);
    ctrReceiptsEmitted++; 
}
#endif

//adaptive milestone limiter implementation
bool DtnOverlayModule::shouldEmitMilestone(NodeNum src, NodeNum dst)
{
    (void)src; (void)dst; // current heuristic does not need identities
    if (!configMilestonesEnabled) return false;
    if (!configMilestoneAutoLimiterEnabled) return true;

    uint8_t chUtil = airTime ? airTime->channelUtilizationPercent() : 0;
    uint8_t neighbors = countDirectNeighbors();
    size_t pend = pendingById.size();

    // Enter suppression when any high threshold exceeded
    if (!runtimeMilestonesSuppressed) {
        if (chUtil >= configMilestoneAutoSuppressHighChUtil ||
            neighbors >= configMilestoneAutoNeighborHigh ||
            pend >= configMilestoneAutoPendingHigh) {
            runtimeMilestonesSuppressed = true;
        }
    } else {
        // Leave suppression only when all metrics are comfortably low (hysteresis)
        if (chUtil <= configMilestoneAutoReleaseLowChUtil &&
            neighbors < configMilestoneAutoNeighborHigh &&
            pend < configMilestoneAutoPendingHigh) {
            runtimeMilestonesSuppressed = false;
        }
    }
    return !runtimeMilestonesSuppressed;
}
void DtnOverlayModule::recordFwplusVersion(NodeNum n, uint16_t version)
{
    fwplusVersionByNode[n] = version;
    markFwplusSeen(n);
    
    //Clear stock marking when we receive FW+ version advertisement
    // This handles the case where a node had DTN disabled (was marked as stock via unresponsive fallback),
    // but later enabled DTN and started sending beacons again
    auto itStock = stockKnownMs.find(n);
    if (itStock != stockKnownMs.end()) {
        LOG_INFO("DTN: Clearing stock marking for node 0x%x (received FW+ version %u beacon)",
                 (unsigned)n, (unsigned)version);
        stockKnownMs.erase(itStock);
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
    // 1. Warmup (first hour): 15min aggressive discovery
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
bool DtnOverlayModule::isNodeReachable(NodeNum node) const
{
    //Guard: broadcast is never reachable as a unicast destination
    if (node == NODENUM_BROADCAST || node == NODENUM_BROADCAST_NO_LORA) {
        return false;
    }
    
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (!info) return false;
    
    // Check if node is not too far
    if (info->hops_away > 3) return false;
    
#ifdef ARCH_PORTDUINO
    // Simulator/testing: no RTC, skip time check - trust routing table
    return true;
#else
    // Check if node was seen recently (use epoch time, not uptime)
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet); // epoch seconds
    uint32_t lastSeenEpoch = info->last_heard; // epoch seconds
    uint32_t maxAgeSec = 30UL * 60UL; // 30 minutes in seconds
    
    // Handle clock not set or very old nodes
    if (nowEpoch == 0 || lastSeenEpoch == 0) return false;
    
    return (nowEpoch - lastSeenEpoch) < maxAgeSec;
#endif
}

// Purpose: intelligent fallback for unknown or low-confidence destinations
// Returns: true if fallback was attempted
bool DtnOverlayModule::tryIntelligentFallback(uint32_t id, Pending &p)
{
    // For stock destinations with low route confidence, try native DM early
    // EXCEPT: for far destinations (>=3 hops), always try DTN overlay first
    // This is the "im dalej w las" scenario - multi-hop native routing is unreliable
    uint8_t hopsToDest = getHopsAway(p.data.orig_to);
    bool isFarDest = (hopsToDest != 255 && hopsToDest >= 3);

    // If the route is unknown (no DV-ETX info yet), suppress native fallback for now
    // This allows DTN discovery (probe/beacons) to establish a path and avoids
    // premature native attempts that can bias routing toward incorrect direct-neighbor
    if (hopsToDest == 255) {
        LOG_DEBUG("DTN: Unknown route to 0x%x - suppressing native fallback this cycle", (unsigned)p.data.orig_to);
        return false; // wait for discovery/probe
    }
    
    if (!isFwplus(p.data.orig_to) && !hasSufficientRouteConfidence(p.data.orig_to)) {
        if (isFarDest) {
            LOG_INFO("DTN: Stock dest 0x%x is far (%u hops) - trying DTN overlay first, fallback later", 
                     (unsigned)p.data.orig_to, (unsigned)hopsToDest);
            return false; // Don't fallback yet, try DTN overlay
        }
        LOG_DEBUG("DTN: Low confidence to stock dest 0x%x, trying native DM", (unsigned)p.data.orig_to);
        return sendProxyFallback(id, p);
    }
    
    // For unreachable destinations, try DTN broadcast as last resort
    if (!isNodeReachable(p.data.orig_to)) {
        // For unknown routes, allow broadcast after first retry (even without valid time)
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        bool completelyUnknown = (hopsToDest == 255);
        bool allowEarlyBroadcast = completelyUnknown && p.tries >= 1;
        
        // TTL-based broadcast: more aggressive for unknown routes (40% vs 20%)
        uint32_t tailPercent = completelyUnknown ? 40 : 20;
        bool allowTtlBroadcast = isInTtlTail(p.data, tailPercent); // helper: handles valid time check
        
        if (allowTtlBroadcast || allowEarlyBroadcast) {
            // Anti-burst: check global cooldown and per-id cooldown before broadcasting
            {
                auto itBroadcast = lastBroadcastSentMs.find(p.data.orig_id);
                uint32_t now = millis();
                // Randomized cooldown 60-120s to allow up to 7-hop delivery before re-broadcast
                uint32_t broadcastCooldown = 60000 + (uint32_t)random(60001);
                
                if (itBroadcast != lastBroadcastSentMs.end() && (now - itBroadcast->second) < broadcastCooldown) {
                    LOG_DEBUG("DTN: Broadcast cooldown active for id=0x%x", p.data.orig_id);
                    return false;
                }
            }
            
            // Channel utilization gate for broadcast
            if (airTime && airTime->channelUtilizationPercent() > 40) {
                LOG_DEBUG("DTN: Channel too busy for broadcast (util=%u%%)", airTime->channelUtilizationPercent());
                return false;
            }
            
            LOG_DEBUG("DTN: Unreachable dest 0x%x in TTL tail, trying DTN broadcast fallback", (unsigned)p.data.orig_to);
            
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
            
            // Ensure packet is not encrypted (public broadcast)
            mp->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
            
            service->sendToMesh(mp, RX_SRC_LOCAL, false);
            p.tries++;
            // Ensure post-broadcast retry spacing is at least 60-120s to avoid storms
            uint32_t minRetry = 60000 + (uint32_t)random(60001);
            uint32_t retryDelay = configRetryBackoffMs > minRetry ? configRetryBackoffMs : minRetry;
            p.nextAttemptMs = millis() + retryDelay;
            lastBroadcastSentMs[p.data.orig_id] = millis(); // Track broadcast time
            LOG_INFO("DTN: DTN broadcast fallback sent for unreachable dest 0x%x", (unsigned)p.data.orig_to);
            return true;
        }
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

    LOG_INFO("DTN FW+ Discovery: %u known nodes, cold_start=%s, last_beacon=%u sec ago, probe_cooldown=%u sec",
              (unsigned)fwplusVersionByNode.size(),
              isDtnCold() ? "YES" : "NO",
              (unsigned)((lastAdvertiseMs == 0 || now < lastAdvertiseMs) ? 0 : (now - lastAdvertiseMs) / 1000),
              getGlobalProbeCooldownRemainingSec());

    //fw+ TEST: Enhanced FW+ discovery diagnostics
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

//FW+ custody handoff target selection
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
            //Check if cached node is still reachable (mesh stability)
            if (itVer != fwplusVersionByNode.end() && 
                itVer->second >= configMinFwplusVersionForHandoff &&
                isNodeReachable(ph.node)) {
                LOG_DEBUG("DTN: Using cached handoff 0x%x for dest 0x%x", (unsigned)ph.node, (unsigned)dest);
                ctrHandoffCacheHits++; //metric
                return ph.node;
            } else {
                LOG_DEBUG("DTN: Cached handoff 0x%x for dest 0x%x is stale/unreachable, rebuilding", 
                         (unsigned)ph.node, (unsigned)dest);
            }
        }
    }

    // 2) PRIORITY: If NextHopRouter has a next_hop that is FW+, use it FIRST (before building shortlist)
    //    This ensures we follow routing table when it has learned the path
    if (router) {
        auto nh = static_cast<NextHopRouter *>(router);
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
            //Enhanced loop detection: check recent carrier history
            if (isCarrierLoop(p, ni->num)) {
                LOG_DEBUG("DTN: Skipping handoff candidate 0x%x - would create loop", (unsigned)ni->num);
                ctrLoopsDetected++; //metric
                continue;
            }
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
                auto nh = static_cast<NextHopRouter *>(router);
                nh->penalizeRouteOnFailed(0, node, 0, meshtastic_Routing_Error_MAX_RETRANSMIT); // Strong penalty for stale route
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
    
    // Handle clock not set or very old nodes
    if (nowEpoch == 0 || lastSeenEpoch == 0) return true; // stale if no valid time
    
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
            
            // CRITICAL: Global rate limiter check
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
uint32_t DtnOverlayModule::calculateTopologyDelay(const meshtastic_FwplusDtnData &d) const
{
    if (configMaxRingsToAct == 0) return 0;
    
    uint8_t hopsToDest = getHopsAway(d.orig_to);
    if (hopsToDest == 255 || hopsToDest <= configMaxRingsToAct) return 0;
    
    if (d.deadline_ms && d.orig_rx_time) {
        uint64_t nowEpoch = getEpochMs(); // helper: safe epoch ms
        
        // Only calculate delay if we have valid time
        if (nowEpoch == 0) {
            return 5000; // fallback: arbitrary delay when no valid time
        }
        
        // Calculate TTL and required wait time
        uint64_t ttl = calculateTtl(d); // helper: safe TTL calculation
        uint64_t mustWait = ttl * (configFarMinTtlFracPercent > 100 ? 100 : configFarMinTtlFracPercent) / 100;
        uint64_t origRxMs = (uint64_t)d.orig_rx_time * 1000ULL;
        uint64_t readyEpoch = origRxMs + mustWait;
        
        if (nowEpoch < readyEpoch) {
            uint64_t delayMs = readyEpoch - nowEpoch;
            //Clamp to uint32 max for return value
            return (delayMs > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (uint32_t)delayMs;
        }
    } else {
        return 5000; // no TTL: arbitrary extra delay when far
    }
    
    return 0;
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
    uint8_t hops = getHopsAway(p.data.orig_to);
    if (hops != 255 && hops <= 1 && shouldUseFallback(p)) {
        if (sendProxyFallback(id, p)) return true;
    }
    return false;
}


// Purpose: try fallback for known stock destinations or TTL tail
bool DtnOverlayModule::tryKnownStockFallback(uint32_t id, Pending &p)
{
    bool destKnownStock = isDestKnownStock(p.data.orig_to);
    
    // Check if we're in TTL tail (helper handles all validity checks)
    bool inTail = configLateFallback && isInTtlTail(p.data, configFallbackTailPercent);
    
    if ((destKnownStock || inTail) && shouldUseFallback(p)) {
        if (inTail && configProbeFwplusNearDeadline) {
            // CRITICAL: Global rate limiter check
            if (!isGlobalProbeCooldownActive()) {
                maybeProbeFwplus(p.data.orig_to);
                updateGlobalProbeTimestamp();
            } else {
                LOG_DEBUG("DTN: TTL tail probe blocked by global rate limiter (need %u sec)", 
                         getGlobalProbeCooldownRemainingSec());
            }
        }
        if (sendProxyFallback(id, p)) return true;
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
    
    // Send native DM fallback
    if (sendProxyFallback(id, p)) {
        LOG_INFO("DTN: Native DM fallback sent to unresponsive FW+ dest 0x%x", (unsigned)p.data.orig_to);
        ctrFwplusUnresponsiveFallbacks++; //increment counter for diagnostics
        
        // Optionally mark this destination as potentially stock for a while
        // This helps avoid repeated DTN attempts to the same unresponsive FW+ node
        stockKnownMs[p.data.orig_to] = millis();
        
        return true;
    }
    
    return false;
}

// Purpose: select the best forward target for DTN packet
NodeNum DtnOverlayModule::selectForwardTarget(Pending &p)
{
    NodeNum target = p.data.orig_to;
    
    if (configEnableFwplusHandoff) {
        uint8_t hopsToDest = getHopsAway(p.data.orig_to);
        if (hopsToDest != 255 && hopsToDest >= configHandoffMinRing) {
            // Try to find handoff candidate - chooseHandoffTarget returns 0 if none available
            NodeNum cand = chooseHandoffTarget(p.data.orig_to, 0, p);
            if (cand != 0 && isValidHandoffCandidate(cand, p.data.orig_to, p)) {
                target = cand;
                LOG_DEBUG("DTN: Using handoff target: 0x%x (hops=%u)", (unsigned)target, (unsigned)hopsToDest);
            } else {
                LOG_DEBUG("DTN: No valid handoff candidates available, using direct");
                // Keep target as orig_to (direct delivery attempt)
            }
        }
    }
    
    return target;
}

// NOTE: triggerImmediateNeighborProbing() removed - rely exclusively on passive broadcast beacons

// NOTE: triggerAggressiveDiscovery() removed - rely exclusively on passive broadcast beacons

// NOTE: triggerTracerouteIfNeededForSource() removed - traceroute now triggered inline in tryForward()

void DtnOverlayModule::setPriorityForTailAndSource(meshtastic_MeshPacket *mp, const Pending &p, bool isFromSource)
{
    mp->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    if (!p.data.deadline_ms) return;
    
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
    
    // TESTING: For destinations >=3 hops away, always use DTN custody
    // This is the "im dalej w las tym wiecej drzew" scenario - multi-hop reliability degrades
    if (ourHopsToDest >= 3) {
        LOG_INFO("DTN: Dest 0x%x is %u hops away (>=3) - using DTN custody", 
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
        auto nh = static_cast<NextHopRouter *>(router);
        auto snap = nh->getRouteSnapshot(false);
        
        for (const auto &e : snap) {
            if (e.dest == dest) {
                // Map next_hop (0-based index) to NodeNum
                meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(e.next_hop);
                if (!node || node->num == 0 || node->num == failedHop) continue;
                
                NodeNum mapped = node->num;
                
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
    
    // Candidate 3: Alternative paths via FW+ neighbors
    for (const auto& entry : fwplusVersionByNode) {
        NodeNum neighbor = entry.first;
        
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
            continue;
        }
        
        // Filter 3: Skip unreliable paths (from historical data)
        if (!isPathReliable(hop, dest)) {
            LOG_DEBUG("DTN AdaptiveReroute: Skip unreliable path via 0x%x", hop);
            continue;
        }
        
        viableCandidates.push_back(candidate);
    }
    
    if (viableCandidates.empty()) {
        LOG_WARN("DTN AdaptiveReroute: All candidate paths filtered out for dest 0x%x", dest);
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
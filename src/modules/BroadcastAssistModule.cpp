#include "BroadcastAssistModule.h"
#include "Default.h"
#include "configuration.h"
#include "gps/GeoCoord.h"

BroadcastAssistModule *broadcastAssistModule;

BroadcastAssistModule::BroadcastAssistModule() : MeshModule("BroadcastAssist")
{
    isPromiscuous = true;  // observe all
    encryptedOk = true;    // see encrypted broadcasts too
    broadcastAssistModule = this;
}

bool BroadcastAssistModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if (!moduleConfig.has_broadcast_assist || !moduleConfig.broadcast_assist.enabled)
        return false;
    // Only consider Lora broadcast frames not from us and not loopback
    if (!isBroadcast(p->to) || isFromUs(p))
        return false;
    // Skip if no hop left
    if (p->hop_limit == 0)
        return false;
    // Only whitelisted ports for decoded payloads
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        if (!isAllowedPort(*p)) return false;
    }
    return true;
}

ProcessMessage BroadcastAssistModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!moduleConfig.has_broadcast_assist || !moduleConfig.broadcast_assist.enabled) return ProcessMessage::CONTINUE;

    //CRITICAL: Check nodeDB before use to prevent NULL dereference crash
    if (!nodeDB) {
        return ProcessMessage::CONTINUE;
    }

    uint32_t now = millis();
    auto *rec = findOrCreate(mp.id, getFrom(&mp), now);
    if (!rec) return ProcessMessage::CONTINUE;

    // Windowing
    uint32_t windowMs = moduleConfig.broadcast_assist.window_ms ? moduleConfig.broadcast_assist.window_ms : 600;
    if (now - rec->firstMs > windowMs) {
        rec->firstMs = now;
        rec->count = 0;
        rec->reflooded = false;
        rec->overheard = false;
    }
    rec->count++;

    // Degree gating (hard or probabilistic)
    uint8_t neighbors = countDirectNeighbors();
    uint32_t degThr = moduleConfig.broadcast_assist.degree_threshold;
    if (degThr) {
        if (neighbors > degThr) {
            //permit amplification if far backbone exists and no overhear yet
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDegree++; return ProcessMessage::CONTINUE; }
        }
    } else {
        // Probabilistic scaling if no hard threshold configured
        float p = computeRefloodProbability(neighbors);
        //Add temporal component for better entropy and avoid synchronization
        uint32_t jitter = ((mp.id ^ nodeDB->getNodeNum()) + (millis() & 0xFF)) & 0xFFFF;
        float r = (float)(jitter) / 65536.0f;
        if (r > p) {
            //permit amplification if far backbone exists and no overhear yet
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDegree++; return ProcessMessage::CONTINUE; }
        }
    }

    // Duplicate suppression with opportunistic backbone exception if not overheard
    uint32_t dupThr = moduleConfig.broadcast_assist.dup_threshold ? moduleConfig.broadcast_assist.dup_threshold : 1;
    if (isBackboneRole() && !rec->overheard) {
        if (rec->count > (dupThr + 1)) {
            //allow one more if targeting far backbone case
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDup++; return ProcessMessage::CONTINUE; }
        }
    } else {
        if (rec->count > dupThr) {
            //allow one more if targeting far backbone case
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDup++; return ProcessMessage::CONTINUE; }
        }
    }

    if (rec->reflooded) return ProcessMessage::CONTINUE;

    // Airtime guard
    if (moduleConfig.broadcast_assist.airtime_guard && !airtimeOk()) { statSuppressedAirtime++; return ProcessMessage::CONTINUE; }

    // Create a copy and rebroadcast with optional jitter and hop clamp
    statRefloodAttempts++;
    meshtastic_MeshPacket *tosend = packetPool.allocCopy(mp);
    if (!tosend) return ProcessMessage::CONTINUE;

    // Decrement hop if applicable
    if (tosend->hop_limit > 0) tosend->hop_limit--;

    // Optional extra hop cap: ensure we don't exceed max_extra_hops addition relative to what we saw
    // Here we simply prevent increasing hops; module never increases hop_limit so this is a no-op guard.
    (void)moduleConfig.broadcast_assist.max_extra_hops;

    //CRITICAL FIX: Safe jitter calculation with overflow protection
    uint32_t jitterConfig = moduleConfig.broadcast_assist.jitter_ms ? moduleConfig.broadcast_assist.jitter_ms : 400;
    if (jitterConfig) {
        uint32_t now = millis();
        //Better entropy: mix packet ID, node num, and time
        uint32_t seed = (mp.id ^ nodeDB->getNodeNum() ^ (now & 0xFFFF));
        uint32_t jitterMs = (seed % (jitterConfig + 1));
        
        //Safe addition with overflow protection
        if (jitterMs > UINT32_MAX - now) {
            // Wraparound would occur - send immediately
            tosend->tx_after = now;
        } else {
            tosend->tx_after = now + jitterMs;
        }
    } else {
        tosend->tx_after = millis();
    }

    tosend->next_hop = NO_NEXT_HOP_PREFERENCE;
    tosend->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    service->sendToMesh(tosend, RX_SRC_LOCAL, false);
    statRefloodSent++;
    lastRefloodMs = millis();
    rec->reflooded = true;
    return ProcessMessage::CONTINUE;
}

BroadcastAssistModule::SeenRec *BroadcastAssistModule::findOrCreate(uint32_t id, uint32_t from, uint32_t nowMs)
{
    // Find existing
    for (int i = 0; i < SEEN_CAP; ++i) {
        if (seen[i].id == id && seen[i].from == from) {
            return &seen[i];
        }
    }
    
    //CRITICAL FIX: Find best slot to reuse - prefer expired slots over oldest
    uint32_t windowMs = moduleConfig.broadcast_assist.window_ms ? 
                        moduleConfig.broadcast_assist.window_ms : 600;
    
    int bestIdx = seenIdx;
    uint32_t oldestTime = seen[seenIdx].firstMs;
    
    // First pass: look for expired slots (outside window)
    for (int i = 0; i < SEEN_CAP; ++i) {
        if (nowMs - seen[i].firstMs > windowMs) {
            // Found expired slot - use it immediately
            bestIdx = i;
            break;
        }
        // Track oldest for fallback
        if (seen[i].firstMs < oldestTime) {
            oldestTime = seen[i].firstMs;
            bestIdx = i;
        }
    }
    
    // Update round-robin index
    seenIdx = (bestIdx + 1) % SEEN_CAP;
    
    // Initialize slot
    SeenRec &slot = seen[bestIdx];
    slot.id = id;
    slot.from = from;
    slot.firstMs = nowMs;
    slot.count = 0;
    slot.reflooded = false;
    slot.overheard = false;
    
    return &slot;
}

uint8_t BroadcastAssistModule::countDirectNeighbors(uint32_t freshnessSecs) const
{
    //CRITICAL: NULL check to prevent crash during boot
    if (!nodeDB) return 0;
    
    uint8_t cnt = 0;
    for (int i = 0; i < nodeDB->numMeshNodes; ++i) {
        const auto &n = nodeDB->meshNodes->at(i);
        if (n.num == nodeDB->getNodeNum()) continue;
        if (sinceLastSeen(&n) <= freshnessSecs) cnt++;
    }
    return cnt;
}

bool BroadcastAssistModule::isAllowedPort(const meshtastic_MeshPacket &mp) const
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return true; // encrypted: allow
    const auto &cfg = moduleConfig.broadcast_assist;
    if (cfg.allowed_ports_count == 0) {
        // default whitelist: TEXT_MESSAGE and POSITION
        //return mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP || mp.decoded.portnum == meshtastic_PortNum_POSITION_APP
        return mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP;
    }
    for (size_t i = 0; i < cfg.allowed_ports_count; ++i) {
        if (cfg.allowed_ports[i] == (uint32_t)mp.decoded.portnum) return true;
    }
    return false;
}

bool BroadcastAssistModule::airtimeOk() const
{
    return (!airTime) || airTime->isTxAllowedChannelUtil(true);
}

//backbone role check used for opportunistic second attempt
bool BroadcastAssistModule::isBackboneRole() const
{
    auto role = config.device.role;
    return role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
           role == meshtastic_Config_DeviceConfig_Role_REPEATER ||
           role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
}

float BroadcastAssistModule::computeRefloodProbability(uint8_t neighborCount) const
{
    // Base probability decreases with neighbor count (dense → lower p)
    // Also scale by (1 - channelUtil) to be polite under load
    float base = 1.0f / (1.0f + (float)neighborCount); // 1, 0.5, 0.33, ...
    float util = airTime ? (airTime->channelUtilizationPercent() / 100.0f) : 0.0f;
    float p = base * (1.0f - util);
    
    //Adaptive boost for sparse networks (< 3 neighbors)
    // This helps ensure messages propagate in very sparse meshes
    if (neighborCount < 3) {
        p = p * 1.5f; // 50% boost for isolated nodes
    }
    
    // Clamp (higher minimum for better propagation)
    if (p < 0.10f) p = 0.10f;      //increased from 0.05 for better coverage
    if (p > 0.95f) p = 0.95f;      //increased from 0.9
    return p;
}

void BroadcastAssistModule::onOverheardFromId(uint32_t from, uint32_t id)
{
    uint32_t now = millis();
    uint32_t windowMs = moduleConfig.broadcast_assist.window_ms ? 
                        moduleConfig.broadcast_assist.window_ms : 600;
    
    //OPTIMIZED: Single pass - find existing OR create new (avoid redundant loop)
    SeenRec *slot = nullptr;
    
    // First check if already exists
    for (int i = 0; i < SEEN_CAP; ++i) {
        if (seen[i].id == id && seen[i].from == from) {
            slot = &seen[i];
            break;
        }
    }
    
    // If not found, create new slot
    if (!slot) {
        slot = findOrCreate(id, from, now);
        if (!slot) return; // Should never happen, but safety check
    }
    
    // Window management
    if (now - slot->firstMs > windowMs) {
        slot->firstMs = now;
        slot->count = 0;
        slot->reflooded = false;
    }
    
    // Mark as overheard
    slot->overheard = true;
}

void BroadcastAssistModule::getStatsSnapshot(BaStatsSnapshot &out) const
{
    out.enabled = moduleConfig.has_broadcast_assist && moduleConfig.broadcast_assist.enabled;
    out.refloodAttempts = statRefloodAttempts;
    out.refloodSent = statRefloodSent;
    out.suppressedDup = statSuppressedDup;
    out.suppressedDegree = statSuppressedDegree;
    out.suppressedAirtime = statSuppressedAirtime;
    //include upstream router duplicate drop count in snapshot
    out.upstreamDupDropped = statUpstreamDupDropped;
    uint32_t now = millis();
    out.lastRefloodAgeSecs = (lastRefloodMs == 0 || now < lastRefloodMs) ? 0 : (now - lastRefloodMs) / 1000;
}

//detect if there exists a far, active backbone node in the DB
bool BroadcastAssistModule::existsActiveFarBackbone(uint32_t minDistanceMeters, uint32_t freshSecs) const
{
    //CRITICAL: NULL check to prevent crash during boot
    if (!nodeDB) return false;
    
    uint32_t now = millis();
    
    // guard: need our own valid position
    meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!self || !nodeDB->hasValidPosition(self)) {
        cachedFarBackboneResult = false;
        return false;
    }
    
    int32_t currentLat = self->position.latitude_i;
    int32_t currentLon = self->position.longitude_i;
    
    //PERFORMANCE: Check if cache is valid:
    // 1. Not expired (within cache time)
    // 2. Position hasn't changed significantly (> 1km = ~10000 units in latitude_i)
    bool cacheValid = false;
    if (now - cachedFarBackboneMs < FAR_BACKBONE_CACHE_MS) {
        int32_t latDiff = currentLat - cachedSelfLatI;
        int32_t lonDiff = currentLon - cachedSelfLonI;
        // Simple Manhattan distance check (~1km threshold)
        if (abs(latDiff) < 10000 && abs(lonDiff) < 10000) {
            cacheValid = true;
        }
    }
    
    if (cacheValid) {
        return cachedFarBackboneResult;
    }
    
    //Cache miss - do expensive calculation
    double selfLat = currentLat * 1e-7;
    double selfLon = currentLon * 1e-7;
    bool foundFarBackbone = false;

    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); ++i) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n) continue;
        if (n->num == nodeDB->getNodeNum()) continue;
        if (!nodeDB->hasValidPosition(n)) continue;
        
        // freshness gate
        if (sinceLastSeen(n) > freshSecs) continue;
        
        // role gate: consider only router/repeater/router_late
        auto role = n->user.role;
        bool isBackbone = role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
                          role == meshtastic_Config_DeviceConfig_Role_REPEATER ||
                          role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
        if (!isBackbone) continue;

        double lat = n->position.latitude_i * 1e-7;
        double lon = n->position.longitude_i * 1e-7;
        float dist = GeoCoord::latLongToMeter(selfLat, selfLon, lat, lon);
        
        if (dist >= (float)minDistanceMeters) {
            foundFarBackbone = true;
            break; //Early exit on first match
        }
    }
    
    //Update cache
    cachedFarBackboneResult = foundFarBackbone;
    cachedFarBackboneMs = now;
    cachedSelfLatI = currentLat;
    cachedSelfLonI = currentLon;
    
    return foundFarBackbone;
}

//decide if we should amplify (permit reflood) targeting far backbone case
bool BroadcastAssistModule::shouldAmplifyForFarBackbone(const SeenRec &rec) const
{
    //CRITICAL FIX: Check overheard first - if router already saw duplicate, don't amplify
    if (rec.overheard) return false;
    
    if (!isBackboneRole()) return false;
    
    // require airtime ok
    if (moduleConfig.broadcast_assist.airtime_guard && !airtimeOk()) return false;
    
    // require presence of an active far backbone (50km+, heard within ~2h)
    if (!existsActiveFarBackbone(50000, 2 * 60 * 60)) return false;
    
    return true;
}

//shim for routers to report upstream duplicate drops without including module headers
void fwplus_ba_onUpstreamDupeDropped()
{
    if (broadcastAssistModule) broadcastAssistModule->onUpstreamDupeDropped();
}

//shim with id/from to mark overheard in BA window
void fwplus_ba_onOverheardFromId(uint32_t from, uint32_t id)
{
    if (!broadcastAssistModule) return;
    broadcastAssistModule->onOverheardFromId(from, id);
}



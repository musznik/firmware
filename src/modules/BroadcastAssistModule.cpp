#include "BroadcastAssistModule.h"
#include "Default.h"
#include "configuration.h"
//fw+ for distance calculation to detect far backbones
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
            //fw+ permit amplification if far backbone exists and no overhear yet
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDegree++; return ProcessMessage::CONTINUE; }
        }
    } else {
        // Probabilistic scaling if no hard threshold configured
        float p = computeRefloodProbability(neighbors);
        // Convert to [0..1) using deterministic jitter from id to avoid rand global state
        uint32_t jitter = (mp.id ^ nodeDB->getNodeNum()) & 0xFFFF;
        float r = (float)(jitter) / 65536.0f;
        if (r > p) {
            //fw+ permit amplification if far backbone exists and no overhear yet
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDegree++; return ProcessMessage::CONTINUE; }
        }
    }

    // Duplicate suppression with opportunistic backbone exception if not overheard
    uint32_t dupThr = moduleConfig.broadcast_assist.dup_threshold ? moduleConfig.broadcast_assist.dup_threshold : 1;
    if (isBackboneRole() && !rec->overheard) {
        if (rec->count > (dupThr + 1)) {
            //fw+ allow one more if targeting far backbone case
            if (!shouldAmplifyForFarBackbone(*rec)) { statSuppressedDup++; return ProcessMessage::CONTINUE; }
        }
    } else {
        if (rec->count > dupThr) {
            //fw+ allow one more if targeting far backbone case
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

    // Jitter
    uint32_t jitter = moduleConfig.broadcast_assist.jitter_ms ? moduleConfig.broadcast_assist.jitter_ms : 400;
    if (jitter) tosend->tx_after = millis() + (random(jitter + 1));

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
    for (int i = 0; i < SEEN_CAP; ++i) if (seen[i].id == id && seen[i].from == from) return &seen[i];
    // Reuse slot
    SeenRec &slot = seen[seenIdx];
    seenIdx = (seenIdx + 1) % SEEN_CAP;
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

//fw+ backbone role check used for opportunistic second attempt
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
    // Clamp
    if (p < 0.05f) p = 0.05f;      // never zero
    if (p > 0.9f) p = 0.9f;        // avoid implosion
    return p;
}

void BroadcastAssistModule::onOverheardFromId(uint32_t from, uint32_t id)
{
    uint32_t now = millis();
    // Try to find quickly; don't allocate new slot just to mark overheard
    for (int i = 0; i < SEEN_CAP; ++i) {
        if (seen[i].id == id && seen[i].from == from) {
            // if window expired, refresh basic fields
            uint32_t windowMs = moduleConfig.broadcast_assist.window_ms ? moduleConfig.broadcast_assist.window_ms : 600;
            if (now - seen[i].firstMs > windowMs) {
                seen[i].firstMs = now;
                seen[i].count = 0;
                seen[i].reflooded = false;
            }
            seen[i].overheard = true;
            return;
        }
    }
    // Optionally prime a slot so that subsequent handleReceived can observe overheard
    SeenRec *slot = findOrCreate(id, from, now);
    if (slot) slot->overheard = true;
}

void BroadcastAssistModule::getStatsSnapshot(BaStatsSnapshot &out) const
{
    out.enabled = moduleConfig.has_broadcast_assist && moduleConfig.broadcast_assist.enabled;
    out.refloodAttempts = statRefloodAttempts;
    out.refloodSent = statRefloodSent;
    out.suppressedDup = statSuppressedDup;
    out.suppressedDegree = statSuppressedDegree;
    out.suppressedAirtime = statSuppressedAirtime;
    //fw+ include upstream router duplicate drop count in snapshot
    out.upstreamDupDropped = statUpstreamDupDropped;
    uint32_t now = millis();
    out.lastRefloodAgeSecs = (lastRefloodMs == 0 || now < lastRefloodMs) ? 0 : (now - lastRefloodMs) / 1000;
}

//fw+ detect if there exists a far, active backbone node in the DB
bool BroadcastAssistModule::existsActiveFarBackbone(uint32_t minDistanceMeters, uint32_t freshSecs) const
{
    // guard: need our own valid position
    meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!self || !nodeDB->hasValidPosition(self)) return false;

    double selfLat = self->position.latitude_i * 1e-7;
    double selfLon = self->position.longitude_i * 1e-7;

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
        if (dist >= (float)minDistanceMeters) return true;
    }
    return false;
}

//fw+ decide if we should amplify (permit reflood) targeting far backbone case
bool BroadcastAssistModule::shouldAmplifyForFarBackbone(const SeenRec &rec) const
{
    (void)rec; // currently only use overhear/role/airtime and far-backbone presence
    if (!isBackboneRole()) return false;
    // require airtime ok
    if (moduleConfig.broadcast_assist.airtime_guard && !airtimeOk()) return false;
    // require presence of an active far backbone (50km+, heard within ~2h)
    if (!existsActiveFarBackbone(50000, 2 * 60 * 60)) return false;
    return true;
}

//fw+ shim for routers to report upstream duplicate drops without including module headers
void fwplus_ba_onUpstreamDupeDropped()
{
    if (broadcastAssistModule) broadcastAssistModule->onUpstreamDupeDropped();
}

//fw+ shim with id/from to mark overheard in BA window
void fwplus_ba_onOverheardFromId(uint32_t from, uint32_t id)
{
    if (!broadcastAssistModule) return;
    broadcastAssistModule->onOverheardFromId(from, id);
}



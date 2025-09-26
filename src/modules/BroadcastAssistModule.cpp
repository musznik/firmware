#include "BroadcastAssistModule.h"
#include "Default.h"
#include "configuration.h"

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
    auto *rec = findOrCreate(mp.id, now);
    if (!rec) return ProcessMessage::CONTINUE;

    // Windowing
    uint32_t windowMs = moduleConfig.broadcast_assist.window_ms ? moduleConfig.broadcast_assist.window_ms : 600;
    if (now - rec->firstMs > windowMs) {
        rec->firstMs = now;
        rec->count = 0;
        rec->reflooded = false;
    }
    rec->count++;

    // Degree check
    uint32_t degThr = moduleConfig.broadcast_assist.degree_threshold;
    if (degThr) {
        uint8_t neighbors = countDirectNeighbors();
        if (neighbors > degThr) { statSuppressedDegree++; return ProcessMessage::CONTINUE; }
    }

    // Duplicate suppression
    uint32_t dupThr = moduleConfig.broadcast_assist.dup_threshold ? moduleConfig.broadcast_assist.dup_threshold : 1;
    if (rec->count > dupThr) { statSuppressedDup++; return ProcessMessage::CONTINUE; }

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

BroadcastAssistModule::SeenRec *BroadcastAssistModule::findOrCreate(uint32_t id, uint32_t nowMs)
{
    // Find existing
    for (int i = 0; i < SEEN_CAP; ++i) if (seen[i].id == id) return &seen[i];
    // Reuse slot
    SeenRec &slot = seen[seenIdx];
    seenIdx = (seenIdx + 1) % SEEN_CAP;
    slot.id = id;
    slot.firstMs = nowMs;
    slot.count = 0;
    slot.reflooded = false;
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
        return mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP || mp.decoded.portnum == meshtastic_PortNum_POSITION_APP;
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

void BroadcastAssistModule::getStatsSnapshot(BaStatsSnapshot &out) const
{
    out.enabled = moduleConfig.has_broadcast_assist && moduleConfig.broadcast_assist.enabled;
    out.refloodAttempts = statRefloodAttempts;
    out.refloodSent = statRefloodSent;
    out.suppressedDup = statSuppressedDup;
    out.suppressedDegree = statSuppressedDegree;
    out.suppressedAirtime = statSuppressedAirtime;
    uint32_t now = millis();
    out.lastRefloodAgeSecs = (lastRefloodMs == 0 || now < lastRefloodMs) ? 0 : (now - lastRefloodMs) / 1000;
}



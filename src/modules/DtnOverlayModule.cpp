
#include "DtnOverlayModule.h"
#if __has_include("mesh/generated/meshtastic/fwplus_dtn.pb.h")
#include "MeshService.h"
#include "Router.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Default.h"
#include "airtime.h"
#include "configuration.h"
#include <pb_encode.h>
#include <cstring>

DtnOverlayModule *dtnOverlayModule; //fw+

void DtnOverlayModule::deliverLocal(const meshtastic_FwplusDtnData &d)
{
    if (d.is_encrypted) {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        memcpy(p->encrypted.bytes, d.payload.bytes, d.payload.size);
        p->encrypted.size = d.payload.size;
        p->channel = d.channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    } else {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = nodeDB->getNodeNum();
        p->from = d.orig_from;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->decoded.payload.size = (d.payload.size > sizeof(p->decoded.payload.bytes)) ? sizeof(p->decoded.payload.bytes) : d.payload.size;
        memcpy(p->decoded.payload.bytes, d.payload.bytes, p->decoded.payload.size);
        p->channel = d.channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }
}

DtnOverlayModule::DtnOverlayModule()
    : concurrency::OSThread("DtnOverlay"),
      ProtobufModule("FwplusDtn", meshtastic_PortNum_FWPLUS_DTN_APP, &meshtastic_FwplusDtn_msg)
{
    //fw+ read config with sensible defaults (moduleConfig.dtn_overlay may not exist yet; use defaults)
    configEnabled = true;
    configTtlMinutes = 5;
    configInitialDelayBaseMs = 8000;
    configRetryBackoffMs = 60000;
    configMaxTries = 3;
    configLateFallback = false;
    configFallbackTailPercent = 20;
    configMilestonesEnabled = false;
    configPerDestMinSpacingMs = 30000;
    configMaxActiveDm = 2;
    configProbeFwplusNearDeadline = false;
    //fw+ DTN: log config snapshot on init
    LOG_INFO("fw+ DTN init: enabled=%d ttl_min=%u initDelayMs=%u backoffMs=%u maxTries=%u lateFallback=%d tail%%=%u milestones=%d perDestMinMs=%u maxActive=%u probeNearDeadline=%d",
             (int)configEnabled, (unsigned)configTtlMinutes, (unsigned)configInitialDelayBaseMs,
             (unsigned)configRetryBackoffMs, (unsigned)configMaxTries, (int)configLateFallback,
             (unsigned)configFallbackTailPercent, (int)configMilestonesEnabled,
             (unsigned)configPerDestMinSpacingMs, (unsigned)configMaxActiveDm,
             (int)configProbeFwplusNearDeadline);
}

int32_t DtnOverlayModule::runOnce()
{
    //fw+ simple scheduler: attempt forwards whose time arrived
    uint32_t now = millis();
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t dmIssuedThisPass = 0; // reset per scheduler pass
    for (auto it = pendingById.begin(); it != pendingById.end();) {
        Pending &p = it->second;
        // Global concurrency cap: throttle attempts
        if (now >= p.nextAttemptMs) {
            if (dmIssuedThisPass < configMaxActiveDm) {
                tryForward(it->first, p);
                dmIssuedThisPass++;
            } else {
                // push a bit forward
                p.nextAttemptMs = now + 1000 + (uint32_t)random(500);
                LOG_DEBUG("fw+ DTN defer(id=0x%x): global cap reached, next in %u ms", it->first, (unsigned)(p.nextAttemptMs - now));
            }
        }
        // Remove if past deadline
        if (p.data.deadline_ms && nowEpoch > p.data.deadline_ms) {
            // emit EXPIRED receipt to source and drop
            LOG_WARN("fw+ DTN expire id=0x%x dl=%u now=%u", it->first, (unsigned)p.data.deadline_ms, (unsigned)nowEpoch);
            emitReceipt(p.data.orig_from, it->first, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_EXPIRED, 0);
            it = pendingById.erase(it);
        } else {
            ++it;
        }
    }
    return 500;
}

bool DtnOverlayModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_FwplusDtn *msg)
{
    if (msg && msg->which_variant == meshtastic_FwplusDtn_data_tag) {
        // capability: mark sender as FW+
        markFwplusSeen(getFrom(&mp));
        // Milestone: jeśli włączone i pierwszy raz widzimy to id od innego węzła – wyślij rare progress do źródła
        if (configMilestonesEnabled) {
            auto it = pendingById.find(msg->variant.data.orig_id);
            if (it == pendingById.end()) {
                // nie mamy pending, ale widzimy cudzy carry – pojedynczy progress
                emitReceipt(msg->variant.data.orig_from, msg->variant.data.orig_id,
                            meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_PROGRESSED, 0);
            }
        }
        LOG_INFO("fw+ DTN rx DATA id=0x%x from=0x%x to=0x%x enc=%d dl=%u", msg->variant.data.orig_id,
                 (unsigned)getFrom(&mp), (unsigned)msg->variant.data.orig_to, (int)msg->variant.data.is_encrypted,
                 (unsigned)msg->variant.data.deadline_ms);
        handleData(mp, msg->variant.data);
        return true;
    } else if (msg && msg->which_variant == meshtastic_FwplusDtn_receipt_tag) {
        markFwplusSeen(getFrom(&mp));
        LOG_INFO("fw+ DTN rx RECEIPT id=0x%x status=%u from=0x%x", msg->variant.receipt.orig_id,
                 (unsigned)msg->variant.receipt.status, (unsigned)getFrom(&mp));
        handleReceipt(mp, msg->variant.receipt);
        return true;
    }
    // Non-DTN packet (decoded==NULL): optionally capture DM into overlay
    // Capture only direct messages we overhear, not from us and not addressed to us //fw+
    if (!isFromUs(&mp) && mp.to != nodeDB->getNodeNum()) { //fw+
        bool isDM = (mp.to != NODENUM_BROADCAST && mp.to != NODENUM_BROADCAST_NO_LORA);
        if (isDM) {
            if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
                enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                                    (getValidTime(RTCQualityFromNet) * 1000UL) + 5 * 60 * 1000UL,
                                    true, mp.encrypted.bytes, mp.encrypted.size, false);
            } else if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
                enqueueFromCaptured(mp.id, getFrom(&mp), mp.to, mp.channel,
                                    (getValidTime(RTCQualityFromNet) * 1000UL) + 5 * 60 * 1000UL,
                                    false, mp.decoded.payload.bytes, mp.decoded.payload.size, false);
            }
        }
    }
    return false; //fw+ do not consume non-DTN packets; allow normal processing
}

void DtnOverlayModule::enqueueFromCaptured(uint32_t origId, uint32_t origFrom, uint32_t origTo, uint8_t channel,
                                           uint32_t deadlineMs, bool isEncrypted, const uint8_t *bytes, pb_size_t size,
                                           bool allowProxyFallback)
{
    meshtastic_FwplusDtnData d = meshtastic_FwplusDtnData_init_zero;
    d.orig_id = origId;
    d.orig_from = origFrom;
    d.orig_to = origTo;
    d.channel = channel;
    d.orig_rx_time = getValidTime(RTCQualityFromNet);
    d.deadline_ms = deadlineMs; // absolute epoch ms
    d.is_encrypted = isEncrypted;
    d.allow_proxy_fallback = allowProxyFallback;
    if (size > sizeof(d.payload.bytes)) size = sizeof(d.payload.bytes);
    memcpy(d.payload.bytes, bytes, size);
    d.payload.size = size;
    LOG_DEBUG("fw+ DTN capture id=0x%x src=0x%x dst=0x%x enc=%d ch=%u ttlms=%u", origId, (unsigned)origFrom,
              (unsigned)origTo, (int)isEncrypted, (unsigned)channel, (unsigned)(deadlineMs));
    scheduleOrUpdate(origId, d);
}

void DtnOverlayModule::handleData(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnData &d)
{
    // If we are the destination, deliver locally
    if (d.orig_to == nodeDB->getNodeNum()) {
        deliverLocal(d);
        // Send DELIVERED receipt back to source
        LOG_INFO("fw+ DTN delivered id=0x%x to=0x%x src=0x%x", d.orig_id, (unsigned)nodeDB->getNodeNum(), (unsigned)d.orig_from);
        emitReceipt(d.orig_from, d.orig_id, meshtastic_FwplusDtnStatus_FWPLUS_DTN_STATUS_DELIVERED, 0);
        return;
    }
    // Otherwise schedule forwarding (do not cancel on first hear until we actually observe overlay forward)
    // Schedule forwarding (or update timing)
    scheduleOrUpdate(d.orig_id, d);
}

void DtnOverlayModule::handleReceipt(const meshtastic_MeshPacket &mp, const meshtastic_FwplusDtnReceipt &r)
{
    // Simplest: stop any pending entry
    (void)mp;
    pendingById.erase(r.orig_id);
}

void DtnOverlayModule::scheduleOrUpdate(uint32_t id, const meshtastic_FwplusDtnData &d)
{
    auto &p = pendingById[id];
    p.data = d;
    p.lastCarrier = nodeDB->getNodeNum();
    // Per-destination spacing: jeśli ostatnio próbowaliśmy do tego samego dest zbyt wcześnie, odłóż
    static std::unordered_map<NodeNum, uint32_t> lastDestTxMs;
    auto itLast = lastDestTxMs.find(d.orig_to);
    // Anti-storm election window: deterministic slot based on id^me
    uint32_t base = configInitialDelayBaseMs ? configInitialDelayBaseMs : 8000;
    uint32_t slots = 8; // small contention window
    uint32_t slotLen = 400; // ms per slot
    uint32_t rank = fnv1a32(id ^ nodeDB->getNodeNum());
    uint32_t slot = rank % slots;
    uint32_t target = millis() + base + slot * slotLen + (uint32_t)random(slotLen);
    if (itLast != lastDestTxMs.end()) {
        uint32_t spacing = configPerDestMinSpacingMs ? configPerDestMinSpacingMs : 30000;
        if (target < itLast->second + spacing) target = itLast->second + spacing + (uint32_t)random(500);
    }
    p.nextAttemptMs = target;
    LOG_DEBUG("fw+ DTN schedule id=0x%x next=%u ms (base=%u slot=%u)", id, (unsigned)(p.nextAttemptMs - millis()), (unsigned)base, (unsigned)slot);
}

void DtnOverlayModule::tryForward(uint32_t id, Pending &p)
{
    // Check channel utilization gate (be polite for overlay)
    bool txAllowed = (!airTime) || airTime->isTxAllowedChannelUtil(true);
    if (!txAllowed) { p.nextAttemptMs = millis() + 2500 + (uint32_t)random(500); LOG_DEBUG("fw+ DTN busy: defer id=0x%x", id); return; }

    // DV-ETX route confidence gating
    if (!hasSufficientRouteConfidence(p.data.orig_to)) {
        // Backoff softly
        p.nextAttemptMs = millis() + configRetryBackoffMs + (uint32_t)random(2000);
        LOG_DEBUG("fw+ DTN low confidence: defer id=0x%x", id);
        return;
    }

    // Optional late proxy fallback to native DM near deadline
    uint32_t nowEpoch = getValidTime(RTCQualityFromNet) * 1000UL;
    uint32_t ttlTailStart = 0;
    if (p.data.deadline_ms && configLateFallback && configFallbackTailPercent)
    {
        uint32_t ttl = (p.data.deadline_ms > p.data.orig_rx_time * 1000UL) ? (p.data.deadline_ms - (p.data.orig_rx_time * 1000UL)) : 0;
        ttlTailStart = p.data.deadline_ms - (ttl * (configFallbackTailPercent > 100 ? 100 : configFallbackTailPercent) / 100);
    }
    bool inTail = (p.data.deadline_ms && nowEpoch >= ttlTailStart);
    if (inTail) {
        // Opcjonalna sonda FW+ tuż przed decyzją o fallbacku
        if (configProbeFwplusNearDeadline && !isFwplus(p.data.orig_to)) {
            maybeProbeFwplus(p.data.orig_to);
        }
    }
    if (inTail && !isFwplus(p.data.orig_to)) {
        // Send native DM (ciphertext or plaintext) towards destination
        meshtastic_MeshPacket *dm = allocDataPacket();
        if (!dm) { p.nextAttemptMs = millis() + 3000; return; }
        dm->to = p.data.orig_to;
        // Preserve oryginalnego nadawcę dla UI po odszyfrowaniu u celu
        dm->from = p.data.orig_from;
        dm->channel = p.data.channel;
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
        }
        //fw+ preserve original DM id so that ROUTING_APP ACK maps to sender's pending entry
        dm->id = p.data.orig_id; //fw+
        dm->want_ack = true; // try to get radio ACK //fw+
        dm->decoded.want_response = false;
        dm->priority = meshtastic_MeshPacket_Priority_DEFAULT;
        service->sendToMesh(dm, RX_SRC_LOCAL, false);
        p.tries++;
        p.nextAttemptMs = millis() + configRetryBackoffMs;
        LOG_INFO("fw+ DTN fallback DM id=0x%x dst=0x%x try=%u", id, (unsigned)p.data.orig_to, (unsigned)p.tries);
        return;
    }

    // Basic forward attempt using overlay again
    meshtastic_FwplusDtn msg = meshtastic_FwplusDtn_init_zero;
    msg.which_variant = meshtastic_FwplusDtn_data_tag;
    msg.variant.data = p.data;

    meshtastic_MeshPacket *mp = allocDataProtobuf(msg);
    if (!mp) {
        p.nextAttemptMs = millis() + 3000;
        return;
    }
    mp->to = p.data.orig_to; // destination node
    mp->decoded.portnum = meshtastic_PortNum_FWPLUS_DTN_APP;
    mp->want_ack = false;
    mp->decoded.want_response = false;
    mp->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    service->sendToMesh(mp, RX_SRC_LOCAL, false);
    p.tries++;
    // update per-destination last tx
    static std::unordered_map<NodeNum, uint32_t> lastDestTxMs;
    lastDestTxMs[p.data.orig_to] = millis();
    // schedule next attempt with backoff
    p.nextAttemptMs = millis() + (configRetryBackoffMs ? configRetryBackoffMs : 60000);
    LOG_INFO("fw+ DTN fwd overlay id=0x%x dst=0x%x try=%u next=%u ms", id, (unsigned)p.data.orig_to, (unsigned)p.tries,
             (unsigned)(p.nextAttemptMs - millis()));
}

void DtnOverlayModule::maybeProbeFwplus(NodeNum dest)
{
    // Minimalna sonda: wyślij krótki overlay receipt z PROGRESSED do DEST jako ping FW+ (nie interpretuje stock)
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
    LOG_DEBUG("fw+ DTN probe FW+ dest=0x%x", (unsigned)dest);
}

void DtnOverlayModule::emitReceipt(uint32_t to, uint32_t origId, meshtastic_FwplusDtnStatus status, uint32_t reason)
{
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
    LOG_DEBUG("fw+ DTN tx RECEIPT id=0x%x status=%u to=0x%x", (unsigned)origId, (unsigned)status, (unsigned)to);
}
#endif



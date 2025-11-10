#include "TextMessageModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "buzz.h"
#include "configuration.h"
#include "main.h"
#include <cmath>
#include <sstream>
#undef round
#define NUM_ONLINE_SECS (60 * 60 * 2) // 2 hrs to consider someone offline
#include "graphics/Screen.h"
TextMessageModule *textMessageModule;

ProcessMessage TextMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    //fw+ use mp.decoded directly to avoid alias not present in non-debug builds
    LOG_INFO("MSG from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, mp.decoded.payload.size, mp.decoded.payload.bytes);
#endif

    // We only store/display messages destined for us.
    // Keep a copy of the most recent text message.
    devicestate.rx_text_message = mp;
    devicestate.has_rx_text_message = true;

    //fw+ fix scope error by referencing mp.decoded directly
    std::string receivedMessage(reinterpret_cast<const char*>(mp.decoded.payload.bytes), mp.decoded.payload.size);

    if(!isBroadcast(mp.to) && mp.to == nodeDB->getNodeNum())
    {
        if(moduleConfig.node_mod_admin.auto_responder_enabled){
            TextMessageModule::sendTextMessage(moduleConfig.node_mod_admin.auto_responder_text, mp, 0);
        }

        if(moduleConfig.node_mod_admin.auto_redirect_messages){
            TextMessageModule::sendTextMessage(receivedMessage, mp, moduleConfig.node_mod_admin.auto_redirect_target_node_id);
        }
    }

    // Only trigger screen wake if configuration allows it
    if (shouldWakeOnReceivedMessage()) {
        powerFSM.trigger(EVENT_RECEIVED_MSG);
    }
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

//fw+ avoid copying mp and keep default arg only in header
void TextMessageModule::sendTextMessage(const std::string &message, const meshtastic_MeshPacket &mp, uint32_t targetId)
{
    //fw+ compute prefix once; app parses this exact shape: "[->] !%08x: "
    auto buildRedirectPrefix = [](char *out, size_t cap, uint32_t fromNode) -> size_t {
        if (!out || cap == 0) return 0;
        int written = snprintf(out, cap, "[->] !%08x: ", fromNode);
        if (written < 0) return 0;
        // Return number of characters excluding the implicit terminator (if any space left)
        size_t len = static_cast<size_t>(written);
        return (len < cap) ? len : cap; // clamp if truncated
    };

    const size_t maxPayloadSize = 200; //fw+ conservative cap respecting LoRa payload and app parsing

    const bool isRedirect = (targetId != 0);
    char prefix[32];
    size_t prefixLen = 0;
    if (isRedirect) {
        //fw+ prefix buffer sized for "[->] !" + 8 hex + ": "
        prefixLen = buildRedirectPrefix(prefix, sizeof(prefix), mp.from);
    }

    size_t remaining = message.size();
    size_t offset = 0;

    while (remaining > 0) {
        //fw+ guard pool exhaustion when allocating send packet (use SinglePortModule helper)
        meshtastic_MeshPacket *p = allocDataPacket();
        if (!p) {
            LOG_WARN("exhausted");
            break;
        }
        p->want_ack = true;

        uint8_t *out = p->decoded.payload.bytes;
        size_t capacity = maxPayloadSize;

        size_t headerLen = 0;
        if (isRedirect && offset == 0) {
            //fw+ put prefix only in the first segment
            headerLen = std::min(prefixLen, capacity);
            if (headerLen) memcpy(out, prefix, headerLen);
        }

        size_t spaceForMsg = (capacity > headerLen) ? (capacity - headerLen) : 0;
        size_t chunkLen = std::min(spaceForMsg, remaining);
        if (chunkLen) memcpy(out + headerLen, message.data() + offset, chunkLen);

        p->decoded.payload.size = headerLen + chunkLen;
        p->to = isRedirect ? targetId : mp.from;

        LOG_INFO("MSG sent id=%d, dest=%x, msg=%.*s", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);
        service->sendToMesh(p);

        offset += chunkLen;
        remaining -= chunkLen;
    }
}

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

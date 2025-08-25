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
    auto &p = mp.decoded;
    LOG_INFO("MSG from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
    // We only store/display messages destined for us.
    // Keep a copy of the most recent text message.
    devicestate.rx_text_message = mp;
    devicestate.has_rx_text_message = true;

    std::string receivedMessage(reinterpret_cast<const char*>(p.payload.bytes), p.payload.size);

    if(!isBroadcast(mp.to) && mp.to == nodeDB->getNodeNum())
    {
        if(moduleConfig.nodemodadmin.auto_responder_enabled){
            TextMessageModule::sendTextMessage(moduleConfig.nodemodadmin.auto_responder_text, mp, 0);
        }

        if(moduleConfig.nodemodadmin.auto_redirect_messages){
            TextMessageModule::sendTextMessage(receivedMessage, mp, moduleConfig.nodemodadmin.auto_redirect_target_node_id);
        }
    }

    // Only trigger screen wake if configuration allows it
    if (shouldWakeOnReceivedMessage()) {
        powerFSM.trigger(EVENT_RECEIVED_MSG);
    }
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

void TextMessageModule::sendTextMessage(const std::string &message, const meshtastic_MeshPacket mp, uint32_t targetId = 0)
{
    std::string finalMessage = message;
    if (targetId != 0) {
        char prefix[32]; // '!' + 8 dec hex + '\0'
        snprintf(prefix, sizeof(prefix), "[->] !%08x: ", mp.from);
        finalMessage = std::string(prefix) + message;
    }
    
    const size_t maxPayloadSize = 200;
    size_t messageLength = finalMessage.size();
    size_t startIndex = 0;

    while (startIndex < messageLength) 
    {
        size_t segmentLength = std::min(maxPayloadSize, messageLength - startIndex);
        std::string segment = finalMessage.substr(startIndex, segmentLength);

        meshtastic_MeshPacket *p = router->allocForSending();
        p->decoded.portnum = mp.decoded.portnum;
        p->want_ack = true;
        p->decoded.payload.size = segment.size();
        memcpy(p->decoded.payload.bytes, segment.c_str(), segment.size());
        p->to = mp.from;

        if (targetId != 0) {
            p->to = targetId;
        }
       
        LOG_INFO("MSG sent id=%d, dest=%x, msg=%.*s", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);
        service->sendToMesh(p);

        startIndex += segmentLength;
    }
}

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}
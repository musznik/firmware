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
TextMessageModule *textMessageModule;

ProcessMessage TextMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#ifdef DEBUG_PORT
    auto &p = mp.decoded;
    LOG_INFO("Received text msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
    // We only store/display messages destined for us.
    // Keep a copy of the most recent text message.
    devicestate.rx_text_message = mp;
    devicestate.has_rx_text_message = true;

    std::string receivedMessage(reinterpret_cast<const char*>(p.payload.bytes), p.payload.size);

    if (receivedMessage == "nodes" || receivedMessage == "nodes all") {
        std::string nodeListMessage = receivedMessage == "nodes" ? "online:\n" : "all:\n";
        int numNodes = nodeDB->getNumMeshNodes();
        bool allNodes = (receivedMessage == "nodes all");

        float channelUtilization = 0.0;
        float airUtilTx = 0.0;

        for (int i = 0; i < numNodes; ++i) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            
            // Skip self
            if (node->num == nodeDB->getNodeNum())
                continue;

            if (!allNodes && sinceLastSeen(node) >= NUM_ONLINE_SECS)
                continue;

            channelUtilization = std::round(node->device_metrics.channel_utilization * 10.0) / 10.0;
            airUtilTx = std::round(node->device_metrics.air_util_tx * 10.0) / 10.0;
 
            nodeListMessage += node->user.long_name;
            nodeListMessage += " ";

            if(channelUtilization>0 && airUtilTx>0){
                nodeListMessage += formatFloatToOneDecimal(channelUtilization) + "%|";
                nodeListMessage += formatFloatToOneDecimal(airUtilTx) + "%";
            }

                        // Add last contact time
            int lastSeenSeconds = sinceLastSeen(node);
            int hours = lastSeenSeconds / 3600;
            int minutes = (lastSeenSeconds % 3600) / 60;
            int seconds = lastSeenSeconds % 60;

            nodeListMessage += " ";
            nodeListMessage += std::to_string(hours) + "h ";
            nodeListMessage += std::to_string(minutes) + "m ";
            nodeListMessage += std::to_string(seconds) + "s ago";

            nodeListMessage += "\n\n";
        }

        TextMessageModule::sendTextMessage(nodeListMessage, mp); 
        powerFSM.trigger(EVENT_RECEIVED_MSG);
        return ProcessMessage::STOP;
    } 
    
    powerFSM.trigger(EVENT_RECEIVED_MSG);
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}


 
void TextMessageModule::sendTextMessage(const std::string &message, const meshtastic_MeshPacket mp)
{
    const size_t maxPayloadSize = 227;  
    size_t messageLength = message.size();
    size_t startIndex = 0;

    while (startIndex < messageLength) {
        size_t segmentLength = std::min(maxPayloadSize, messageLength - startIndex);
        std::string segment = message.substr(startIndex, segmentLength);

        meshtastic_MeshPacket *p = router->allocForSending();
        p->decoded.portnum = mp.decoded.portnum;
        p->want_ack = false;
        p->decoded.payload.size = segment.size();
        memcpy(p->decoded.payload.bytes, segment.c_str(), segment.size());
        p->to = mp.from;

        LOG_INFO("Send message id=%d, dest=%x, msg=%.*s", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);
        service->sendToMesh(p);

        startIndex += segmentLength;
    }
}

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

std::string TextMessageModule::formatFloatToOneDecimal(float value) {
    std::ostringstream oss;
    oss.precision(1);
    oss << std::fixed << value;
    return oss.str();
}
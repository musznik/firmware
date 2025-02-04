#include "OnDemandModule.h"
#include "../mesh/generated/meshtastic/ondemand.pb.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include <meshUtils.h>
#include <pb_encode.h>

OnDemandModule *onDemandModule;
static const int MAX_NODES_PER_PACKET = 10;
static const int MAX_PACKET_SIZE = 160;
#define NUM_ONLINE_SECS (60 * 60 * 2) 

int32_t OnDemandModule::runOnce()
{
    return default_broadcast_interval_secs;
}

bool OnDemandModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *t)
{
    if (t->which_variant == meshtastic_OnDemand_request_tag) 
    {
        if(t->variant.request.request_type == meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY)
        {
            meshtastic_OnDemand od = prepareRxPacketHistory();
            sendPacketToRequester(od,mp.from);
        }

        if(t->variant.request.request_type == meshtastic_OnDemandType_REQUEST_NODES_ONLINE)
        {
            auto packets = createSegmentedNodeList();

            for (auto &pkt : packets)
            {
                sendPacketToRequester(*pkt, mp.from);
                vTaskDelay(10000 / portTICK_PERIOD_MS);
            }
            return true;
        }

        if(t->variant.request.request_type == meshtastic_OnDemandType_REQUEST_PING)
        {
            meshtastic_OnDemand od = preparePingResponse();
            sendPacketToRequester(od, mp.from);
        }
    }

    return false; // Let others look at this message also if they want
}

bool OnDemandModule::fitsInPacket(const meshtastic_OnDemand &onDemand, size_t maxSize)
{
    // temp buff
    uint8_t buffer[512];

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, meshtastic_OnDemand_fields, &onDemand))
    {
        return false;
    }

    return (stream.bytes_written <= maxSize);
}

/// Given a node, return how many seconds in the past (vs now) that we last heard from it
uint32_t OnDemandModule::sinceLastSeen(const meshtastic_NodeInfoLite *n)
{
    uint32_t now = getTime();

    int delta = (int)(now - n->last_heard);
    if (delta < 0)
        delta = 0;

    return delta;
}

std::vector<std::unique_ptr<meshtastic_OnDemand>> OnDemandModule::createSegmentedNodeList()
{
    std::vector<std::unique_ptr<meshtastic_OnDemand>> packets;

    int totalNodes = nodeDB->getNumMeshNodes();
    int currentIndex = 0;
    int packetIndex = 1;

    while (currentIndex < totalNodes)
    {
        std::unique_ptr<meshtastic_OnDemand> onDemand(new meshtastic_OnDemand);
        *onDemand = meshtastic_OnDemand_init_zero;

        onDemand->which_variant = meshtastic_OnDemand_response_tag;
        onDemand->variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODES_ONLINE;
        onDemand->variant.response.which_response_data = meshtastic_OnDemandResponse_node_list_tag;

        onDemand->packet_index = packetIndex;
        meshtastic_NodesList &listRef = onDemand->variant.response.response_data.node_list;
        listRef.node_list_count = 0;

        while (currentIndex < totalNodes)
        {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(currentIndex);
            meshtastic_NodeEntry entry = meshtastic_NodeEntry_init_zero;

             if (sinceLastSeen(node) >= NUM_ONLINE_SECS){
                currentIndex++;
                continue;
             }
               
            entry.node_id = node->num;
            entry.last_heard = sinceLastSeen(node);   
            entry.snr = node->snr;

            strncpy(entry.long_name, node->user.long_name, sizeof(entry.long_name) - 1);
            entry.long_name[sizeof(entry.long_name) - 1] = '\0';

            strncpy(entry.short_name, node->user.short_name, sizeof(entry.short_name) - 1);
            entry.short_name[sizeof(entry.short_name) - 1] = '\0';

            int pos = listRef.node_list_count;
            listRef.node_list[pos] = entry;
            listRef.node_list_count++;

            if (!fitsInPacket(*onDemand, MAX_PACKET_SIZE))
            {
                listRef.node_list_count--;
                break;
            }
            currentIndex++;
        }

        packets.push_back(std::move(onDemand));

        packetIndex++;
    }

    uint32_t totalPackets = packetIndex - 1;
    for (auto &pkt : packets)
    {
        pkt->packet_total = totalPackets;
    }

    return packets;
}

meshtastic_OnDemand OnDemandModule::prepareNodeList(uint32_t packetIndex)
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;

    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODES_ONLINE;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_node_list_tag;

    int totalNodes = nodeDB->getNumMeshNodes();

    int totalPackets = (totalNodes + MAX_NODES_PER_PACKET - 1) / MAX_NODES_PER_PACKET;


    onDemand.packet_index = packetIndex + 1;
    onDemand.packet_total = totalPackets;

    int startNodeIndex = packetIndex * MAX_NODES_PER_PACKET;
    int endNodeIndex = std::min(startNodeIndex + MAX_NODES_PER_PACKET, totalNodes);
    int sliceCount = endNodeIndex - startNodeIndex;

    meshtastic_NodesList &listRef = onDemand.variant.response.response_data.node_list;

    listRef.node_list_count = sliceCount;

    for (int i = 0; i < sliceCount; i++)
    {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(startNodeIndex + i);
        meshtastic_NodeEntry entry = meshtastic_NodeEntry_init_zero;

        entry.node_id = node->num; 
        entry.last_heard = getLastHeard(node); 
        entry.snr = node->snr; 

        strncpy(entry.long_name, node->user.long_name, sizeof(entry.long_name) - 1);
        entry.long_name[sizeof(entry.long_name) - 1] = '\0';

        strncpy(entry.short_name, node->user.short_name, sizeof(entry.short_name) - 1);
        entry.short_name[sizeof(entry.short_name) - 1] = '\0';

        listRef.node_list[i] = entry;
    }

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePingResponse()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.packet_index = 1;
    onDemand.packet_total = 1;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PING;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_ping_tag;
    onDemand.variant.response.response_data.ping.direct=false;
 
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRxPacketHistory()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.packet_index = 1;
    onDemand.packet_total = 1;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PACKET_RX_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_rx_packet_history_tag;
    onDemand.variant.response.response_data.rx_packet_history.rx_packet_history_count = moduleConfig.nodemodadmin.rx_packet_history_count;
 
    memcpy(onDemand.variant.response.response_data.rx_packet_history.rx_packet_history, moduleConfig.nodemodadmin.rx_packet_history, moduleConfig.nodemodadmin.rx_packet_history_count * sizeof(uint32_t));

    return onDemand;
}

void OnDemandModule::sendPacketToRequester(meshtastic_OnDemand demand_packet,u_int32_t from){
    meshtastic_MeshPacket *p = allocDataProtobuf(demand_packet);
    p->to = from;
    p->decoded.want_response = false;
    p->want_ack=false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;    
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

meshtastic_MeshPacket *OnDemandModule::allocReply()
{
    return NULL;
}
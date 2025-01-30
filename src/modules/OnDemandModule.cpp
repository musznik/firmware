#include "OnDemandModule.h"
#include "../mesh/generated/meshtastic/ondemand.pb.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#include <meshUtils.h>

OnDemandModule *onDemandModule;

int32_t OnDemandModule::runOnce()
{
    return default_broadcast_interval_secs;
}

bool OnDemandModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *t)
{
    LOG_WARN("OnDemandModule::handleReceivedProtobuf");
    if (t->which_variant == meshtastic_OnDemand_request_tag) {
        LOG_WARN("OnDemandModule:: == meshtastic_OnDemand_request_tag");
        if(t->variant.request.request_type == meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY){
            LOG_WARN("OnDemandModule:: == variant.request.request_type == meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY");
            sendPacketToRequester(prepareRxPacketHistory(),mp.from);
        }
    }

    return false; // Let others look at this message also if they want
}

meshtastic_OnDemand OnDemandModule::prepareRxPacketHistory()
{   
    LOG_WARN("exec prepareRxPacketHistory");
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
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;    
    LOG_WARN("SENDING OnDemandModule::sendPacketToRequester");
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

meshtastic_MeshPacket *OnDemandModule::allocReply()
{
    return NULL;
}

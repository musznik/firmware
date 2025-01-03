#include "NodeMod.h"
#include "../mesh/generated/meshtastic/node_mod.pb.h"
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

NodeModModule *nodeModModule;

int32_t NodeModModule::runOnce()
{
    refreshUptime();
    sendToPhone();
    sendToMesh();
    return 120 * 1000;
}

bool NodeModModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_NodeMod *t)
{
    return true; // Let others look at this message also if they want
}

meshtastic_MeshPacket *NodeModModule::allocReply()
{
    return NULL;
}

void NodeModModule::adminChangedStatus(){
    refreshUptime();
    sendToMesh();
}

meshtastic_MeshPacket* NodeModModule::preparePacket(){
    meshtastic_NodeMod nodemod = meshtastic_NodeMod_init_zero;
    strncpy(nodemod.text_status, moduleConfig.nodemod.text_status, sizeof(nodemod.text_status) - 1);
    meshtastic_MeshPacket *p = allocDataProtobuf(nodemod);
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = static_cast<meshtastic_MeshPacket_Priority>(22);
    return p;
}

void NodeModModule::sendToPhone()
{
    if (lastSentStatsToPhone == 0 || uptimeLastMs - lastSentStatsToPhone >= 360*1000) {
        meshtastic_MeshPacket* packet = preparePacket();
        service->sendToPhone(packet);    
        lastSentStatsToPhone = uptimeLastMs;
    }
}
 
void NodeModModule::sendToMesh()
{
    if (lastSentToMesh == 0 || (uptimeLastMs - lastSentToMesh >= (1800 * 1000) && airTime->isTxAllowedAirUtil()))
    {
        if (strlen(moduleConfig.nodemod.text_status) == 0) {
            return;
        }

        meshtastic_MeshPacket* packet = preparePacket();
        service->sendToMesh(packet, RX_SRC_LOCAL, true);
        lastSentToMesh = uptimeLastMs;
    }
}
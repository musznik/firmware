#include "NodeMod.h"
#include "../mesh/generated/meshtastic/node_mod.pb.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include <meshUtils.h>

NodeModModule *nodeModModule;

int32_t NodeModModule::runOnce()
{
    refreshUptime();
    
    if (firstTime) {
        // First run after boot - send to phone quickly, but wait for radio
        firstTime = false;
        sendToPhone(true); // Force send to phone on first run
        LOG_INFO("NodeMod: Initialized, sent to phone. Will broadcast to radio in 2-4 min");
        return (120 + random(120)) * 1000; // 2-4 minutes until first radio broadcast
    }
    
    // Normal operation - send to phone and mesh
    sendToPhone(false);
    sendToMesh(false);
    return 120 * 1000;
}

bool NodeModModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_NodeMod *t)
{
    return false; // Let others look at this message also if they want
}

meshtastic_MeshPacket *NodeModModule::allocReply()
{
    return NULL;
}

void NodeModModule::adminChangedStatus(){
    refreshUptime();
    sendToPhone(true); // Force immediate send to phone on status change
    sendToMesh(true);  // Send to mesh with statusChanged=true (has throttle)
}

meshtastic_MeshPacket* NodeModModule::preparePacket(){
    meshtastic_NodeMod nodemod = meshtastic_NodeMod_init_zero;
    strncpy(nodemod.text_status, moduleConfig.nodemod.text_status, sizeof(nodemod.text_status));

    if(strlen(moduleConfig.nodemod.emoji) != 0){
        nodemod.has_emoji=true;
        strncpy(nodemod.emoji, moduleConfig.nodemod.emoji, sizeof(nodemod.emoji));
    }

    meshtastic_MeshPacket *p = allocDataProtobuf(nodemod);
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    return p;
}

void NodeModModule::sendToPhone(bool force)
{
    // Force send immediately (for status changes), or send if enough time has passed
    if (force || (lastSentStatsToPhone == 0) || (uptimeLastMs - lastSentStatsToPhone >= 360*1000)) {
        meshtastic_MeshPacket* packet = preparePacket();
        service->sendToPhone(packet);    
        lastSentStatsToPhone = uptimeLastMs;
        LOG_DEBUG("NodeMod: Sent to phone (force=%d)", force);
    }
}
 
void NodeModModule::sendToMesh(bool statusChanged)
{
    uint32_t baseIntervalMs = Default::getConfiguredOrDefaultMsScaled(0, (default_telemetry_broadcast_interval_secs*1.3), numOnlineNodes);
    uint32_t statusChangeIntervalMs = Default::getConfiguredOrDefaultMsScaled(0, 300, numOnlineNodes);

    bool allowedByDefault = ((lastSentToMesh == 0) || (((uptimeLastMs - lastSentToMesh) >= baseIntervalMs) && airTime->isTxAllowedAirUtil() && airTime->isTxAllowedChannelUtil(false)));
    bool allowedDueToStatusChange = (statusChanged == true && ((uptimeLastMs - lastSentToMesh) >= statusChangeIntervalMs) && airTime->isTxAllowedAirUtil() && airTime->isTxAllowedChannelUtil(false));

    if (allowedByDefault || allowedDueToStatusChange)
    {
        if (strlen(moduleConfig.nodemod.text_status) == 0 && strlen(moduleConfig.nodemod.emoji) == 0) {
            return;
        }

        meshtastic_MeshPacket* packet = preparePacket();
        packet->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
        service->sendToMesh(packet, RX_SRC_LOCAL, true);
        lastSentToMesh = uptimeLastMs;
    }
}
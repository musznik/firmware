#pragma once
#include "../mesh/generated/meshtastic/node_mod.pb.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
const uint8_t IDENTIFIER[] = {0xF9, 0x00};  
 
class NodeModModule : private concurrency::OSThread, public ProtobufModule<meshtastic_NodeMod>
{

    CallbackObserver<NodeModModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<NodeModModule, const meshtastic::Status *>(this, &NodeModModule::handleStatusUpdate);

  public:
    NodeModModule()
        : concurrency::OSThread("NodeMod"),
          ProtobufModule("NodeMod", meshtastic_PortNum_NODE_MOD_APP, &meshtastic_NodeMod_msg)
    {
        uptimeWrapCount = 0;
        uptimeLastMs = millis();
        refreshUptime();
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(10 * 1000); // Quick first run to send to phone
    }

    void adminChangedStatus();

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_NodeMod *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    /**
     * Send our Telemetry into the mesh
     */
    void sendToMesh(bool statusChanged);
    void sendToPhone(bool force = false);
    meshtastic_MeshPacket* preparePacket();

    /**
     * Get the uptime in seconds
     * Loses some accuracy after 49 days, but that's fine
     */
    uint32_t getUptimeSeconds() { return (0xFFFFFFFF / 1000) * uptimeWrapCount + (uptimeLastMs / 1000); }

  private:
    uint32_t lastSentToMesh = 0;
    uint32_t lastSentStatsToPhone = 0;
    bool statsHaveBeenSent = false;
    bool firstTime = true;

    void refreshUptime()
    {
        auto now = millis();
        // If we wrapped around (~49 days), increment the wrap count
        if (now < uptimeLastMs)
            uptimeWrapCount++;

        uptimeLastMs = now;
    }

    uint32_t uptimeWrapCount;
    uint32_t uptimeLastMs;
};

extern NodeModModule *nodeModModule;
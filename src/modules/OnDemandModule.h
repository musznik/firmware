#pragma once
#include "../mesh/generated/meshtastic/ondemand.pb.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
class OnDemandModule : private concurrency::OSThread, public ProtobufModule<meshtastic_OnDemand>
{

    CallbackObserver<OnDemandModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<OnDemandModule, const meshtastic::Status *>(this, &OnDemandModule::handleStatusUpdate);

  public:
    OnDemandModule()
        : concurrency::OSThread("OnDemand"),
          ProtobufModule("OnDemand", meshtastic_PortNum_ON_DEMAND_APP, &meshtastic_OnDemand_msg)
    {
        uptimeWrapCount = 0;
        uptimeLastMs = millis();
        refreshUptime();
    
        //nodeStatusObserver.observe(&nodeStatus->onNewStatus);
    }
 
  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;

 
    /**
     * Get the uptime in seconds
     * Loses some accuracy after 49 days, but that's fine
     */
    uint32_t getUptimeSeconds() { return (0xFFFFFFFF / 1000) * uptimeWrapCount + (uptimeLastMs / 1000); }

    meshtastic_OnDemand prepareRxPacketHistory();
 
    void sendPacketToRequester(const meshtastic_OnDemand &demand_packet,const meshtastic_MeshPacket &mp, bool wantAck=true);

    bool fitsInPacket(const meshtastic_OnDemand &onDemand, size_t maxSize);
    std::vector<std::unique_ptr<meshtastic_OnDemand>> createSegmentedNodeList(bool directOnly = false);
    std::vector<std::unique_ptr<meshtastic_OnDemand>> createSegmentedRoutingTable();
    meshtastic_OnDemand prepareNodeList(uint32_t packetIndex);
    static uint32_t getLastHeard(const meshtastic_NodeInfoLite* node);
    uint32_t sinceLastSeen(const meshtastic_NodeInfoLite *n);
    meshtastic_OnDemand preparePingResponse(const meshtastic_MeshPacket &mp);
    meshtastic_OnDemand prepareRxAvgTimeHistory();
    meshtastic_OnDemand preparePortCounterHistory();
    meshtastic_OnDemand preparePacketHistoryLog();
    meshtastic_OnDemand prepareAirActivityHistoryLog();
    meshtastic_OnDemand prepareNodeStats();
    meshtastic_OnDemand prepareFwPlusVersion();
    meshtastic_OnDemand prepareRoutingErrorResponse();
    meshtastic_OnDemand preparePingResponseAck(const meshtastic_MeshPacket &mp);
    //fw+ S&F status
    meshtastic_OnDemand prepareSFCustodyStatus();
    //fw+ DTN overlay stats
    meshtastic_OnDemand prepareDtnOverlayStats();
    //fw+ Broadcast Assist stats
    meshtastic_OnDemand prepareBroadcastAssistStats();

  private:
    uint32_t lastSentToMesh = 0;
    uint32_t lastSentStatsToPhone = 0;
    bool statsHaveBeenSent = false;

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

extern OnDemandModule *onDemandModule;
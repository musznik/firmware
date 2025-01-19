#pragma once
#include "ProtobufModule.h"
#include "../mesh/generated/meshtastic/idlegame.pb.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"

class IdleGameModule : private concurrency::OSThread, public ProtobufModule<meshtastic_IdleGame>
{

    CallbackObserver<IdleGameModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<IdleGameModule, const meshtastic::Status *>(this, &IdleGameModule::handleStatusUpdate);

public:
    IdleGameModule()  : concurrency::OSThread("IdleGame"),
          ProtobufModule("IdleGame", meshtastic_PortNum_IDLE_GAME_APP, &meshtastic_IdleGame_msg)
    {
        uptimeWrapCount = 0;
        uptimeLastMs = millis();
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);

        // moduleConfig.idlegame.variant.state.village_name="NiceVill One";
        // moduleConfig.idlegame.variant.state.population=1;
        // moduleConfig.idlegame.variant.state.resources=5;
        // moduleConfig.idlegame.variant.state.defense=1;
        // moduleConfig.idlegame.variant.state.technology=1;

        setIntervalFromNow(45 * 1000);
    }

    virtual int32_t runOnce() override;
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_IdleGame *t) override;
    void sendIdleGameState(NodeNum dest = NODENUM_BROADCAST, bool requestAck = false);
    void sendIdleGameAction(meshtastic_IdleGameAction &action);
    bool addOrUpdateKnownVillage(const meshtastic_IdleGameState &receivedState);

private:
    bool checkChance(float chancePercent);
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

    unsigned long lastStateBroadcastMs = 0;
};

extern IdleGameModule *idleGameModule;
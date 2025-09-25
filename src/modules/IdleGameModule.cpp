#include "IdleGameModule.h"
#include "../mesh/generated/meshtastic/idlegame.pb.h"
#include "RTC.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"

IdleGameModule *idleGameModule;
 
int32_t IdleGameModule::runOnce()
{
    refreshUptime();

    if (uptimeLastMs - lastStateBroadcastMs > 60 * 1000) { // co 60s
        sendIdleGameState(); 
        lastStateBroadcastMs = uptimeLastMs;
    }

    //resource income
    if (uptimeLastMs - lastStateBroadcastMs > 60 * 1000) { // co 60s
         moduleConfig.idlegame.variant.state.resources +=1;
    }

    if(moduleConfig.idlegame.variant.state.population == 0 || !moduleConfig.idlegame.variant.state.population){
        moduleConfig.idlegame.variant.state.population = 1;
        moduleConfig.idlegame.variant.state.resources = 5;
        moduleConfig.idlegame.variant.state.technology = 1;
        moduleConfig.idlegame.variant.state.defense = 1;
        moduleConfig.idlegame.variant.state.node_id = 1;
        // snprintf(moduleConfig.idlegame.variant.state.village_name,
        //         sizeof(moduleConfig.idlegame.variant.state.village_name),
        //         "Village-%s",
        //         devicestate.owner.long_name);
    }

     return SECONDS_IN_MINUTE * 10000; // 10min
}

bool checkChance(float chancePercent)
{
    if (chancePercent <= 0.0f) {
        return false;
    } else if (chancePercent >= 100.0f) {
        return true;
    }

    int r = random(100);

    return (r < chancePercent);
}


bool IdleGameModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_IdleGame *t)
{
    switch (mp.which_payload_variant) {
        case meshtastic_ModuleConfig_IdleGameConfig_state_tag: {
            meshtastic_IdleGameState receivedState = t->variant.state;
            //fw+ silence unused-but-set warning until state handling is implemented
            (void)receivedState;
            // LOG_INFO("IdleGame: received state: %s, ppopulation=%u",
            //          receivedState.village_name,
            //          receivedState.population);

            //nodeDB->updateIdleGameState(getFrom(&mp), receivedState);
            break;
        }
        case meshtastic_ModuleConfig_IdleGameConfig_action_tag: {
            meshtastic_IdleGameAction receivedAction = t->variant.action;
            // LOG_INFO("IdleGame: action received: %s => %s, typ=%d, qty=%u",
            //          receivedAction.from_node_id_village,
            //          receivedAction.to_node_id_village,
            //          receivedAction.action_type,
            //          receivedAction.quantity);

            if (receivedAction.to_node_id_village == myNodeInfo.my_node_num) {
                switch (receivedAction.action_type) {
                    case meshtastic_IdleGameActionType_ACTION_ATTACK:
                        if (moduleConfig.idlegame.variant.state.defense >= receivedAction.quantity) {
                            //LOG_INFO("IdleGame: defense succeed!");
                            moduleConfig.idlegame.variant.state.defense = moduleConfig.idlegame.variant.state.defense - (receivedAction.quantity/4);
                        } else {
                            //LOG_INFO("IdleGame: we lost...");
                            moduleConfig.idlegame.variant.state.defense = receivedAction.quantity/2;
                        }
                        break;
                    case meshtastic_IdleGameActionType_ACTION_TRADE:
                        moduleConfig.idlegame.variant.state.resources += receivedAction.quantity;
                        //LOG_INFO("Przyjęto handel. Zyskujemy %u surowców", receivedAction.quantity);
                        break;
                    default:
                        //LOG_INFO("Otrzymano inną akcję: %d", receivedAction.action_type);
                        break;
                }
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool IdleGameModule::addOrUpdateKnownVillage(const meshtastic_IdleGameState &receivedState)
{
    for (int i = 0; i < moduleConfig.idlegame.variant.known_villages.known_count; i++)
    {
        meshtastic_ModuleConfig_IdleGameState kv = meshtastic_IdleGameState_init_zero;
        kv = moduleConfig.idlegame.variant.known_villages.known[i];

        //update village
        if (kv.node_id == receivedState.node_id) {
            kv.population  = receivedState.population;
            kv.resources   = receivedState.resources;
            kv.defense     = receivedState.defense;
            kv.technology  = receivedState.technology;
            kv.node_id     = receivedState.node_id;

            LOG_INFO("IdleGame: Updated known village: %s", kv.village_name);
            return true;
        }
    }

    //new village
    if (moduleConfig.idlegame.variant.known_villages.known_count < 20) {
        int idx = moduleConfig.idlegame.variant.known_villages.known_count;
        meshtastic_ModuleConfig_IdleGameState kv = moduleConfig.idlegame.variant.known_villages.known[idx];

        strncpy(kv.village_name, receivedState.village_name, sizeof(kv.village_name) - 1);
        kv.village_name[sizeof(kv.village_name) - 1] = '\0';
        kv.population  = receivedState.population;
        kv.resources   = receivedState.resources;
        kv.defense     = receivedState.defense;
        kv.technology  = receivedState.technology;
        kv.node_id     = receivedState.node_id;

        moduleConfig.idlegame.variant.known_villages.known_count++;
        // LOG_INFO("IdleGame: Added new known village: %s (total now=%d)",
        //          kv.village_name, moduleConfig.idlegame.variant.known_villages.known_count);
        return true;
    } else {
       // LOG_WARN("IdleGame: known_villages is full, can't add new city");
        return false;
    }
}

void IdleGameModule::sendIdleGameState(NodeNum dest, bool requestAck)
{
    meshtastic_IdleGame idleGame = meshtastic_IdleGame_init_zero;
    idleGame.which_variant = meshtastic_IdleGame_state_tag;

    idleGame.variant.state.defense = moduleConfig.idlegame.variant.state.defense;
    idleGame.variant.state.population = moduleConfig.idlegame.variant.state.population;
    idleGame.variant.state.resources = moduleConfig.idlegame.variant.state.resources;
    idleGame.variant.state.technology = moduleConfig.idlegame.variant.state.technology;
    idleGame.variant.state.node_id = devicestate.my_node.my_node_num;

    strncpy(idleGame.variant.state.village_name,
        moduleConfig.idlegame.variant.state.village_name,
        sizeof(idleGame.variant.state.village_name));

    meshtastic_MeshPacket *p = allocDataProtobuf(idleGame);

    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
   // LOG_WARN("IdleGame: sending my village status %s - broadcast", moduleConfig.idlegame.variant.state.village_name, dest);

    service->sendToMesh(p, RX_SRC_LOCAL, false);
}

void IdleGameModule::sendIdleGameAction(meshtastic_IdleGameAction &action)
{
    meshtastic_IdleGame idleGame = meshtastic_IdleGame_init_zero;
    idleGame.which_variant = meshtastic_IdleGame_action_tag;
    idleGame.variant.action = action;
 
    meshtastic_MeshPacket *p = allocDataProtobuf(idleGame);
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    //LOG_INFO("IdleGame: sending action %d from %s to %s", action.action_type, action.from_node_id_village, action.to_node_id_village);

    service->sendToMesh(p, RX_SRC_LOCAL, true);
}
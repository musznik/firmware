#include "DeviceTelemetry.h"
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include "memGet.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#include <meshUtils.h>
#include "FSCommon.h"
#include "SPILock.h"

#define MAGIC_USB_BATTERY_LEVEL 101

int32_t DeviceTelemetryModule::runOnce()
{
    refreshUptime();
    bool isImpoliteRole =
        IS_ONE_OF(config.device.role, meshtastic_Config_DeviceConfig_Role_SENSOR, meshtastic_Config_DeviceConfig_Role_ROUTER);
    if (((lastSentToMesh == 0) ||
         ((uptimeLastMs - lastSentToMesh) >=
          Default::getConfiguredOrDefaultMsScaled(moduleConfig.telemetry.device_update_interval,
                                                  default_telemetry_broadcast_interval_secs, numOnlineNodes))) &&
        airTime->isTxAllowedChannelUtil(!isImpoliteRole) && airTime->isTxAllowedAirUtil() &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN &&
        moduleConfig.telemetry.device_telemetry_enabled) {
        sendTelemetry();
        lastSentToMesh = uptimeLastMs;
    } else if (service->isToPhoneQueueEmpty()) {
        // Just send to phone when it's not our time to send to mesh yet
        // Only send while queue is empty (phone assumed connected)
        sendTelemetry(NODENUM_BROADCAST, true);
        if (lastSentStatsToPhone == 0 || (uptimeLastMs - lastSentStatsToPhone) >= sendStatsToPhoneIntervalMs) {
            sendLocalStatsToPhone();
            lastSentStatsToPhone = uptimeLastMs;
        }
    }

    // send local telemetry some time after normal telemetry (lightly scaled by network size)
    if (statsHaveBeenSent == true &&
        localStatsHaveBeenSent == false &&
        (uptimeLastMs - lastSentToMesh) >= Default::getLightlyScaledWindowMs(5 * 60, numOnlineNodes) &&
        airTime->isTxAllowedChannelUtil(!isImpoliteRole) &&
        airTime->isTxAllowedAirUtil() &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN)
    {
        if(moduleConfig.node_mod_admin.local_stats_over_mesh_enabled){
            sendLocalStatsToMesh();
        }

        localStatsHaveBeenSent=true;
        lastSentLocalStatsToMesh=uptimeLastMs;
    }

    // send local telemetry extended some time after local telemetry over mesh (lightly scaled by network size)
    if (statsHaveBeenSent == true &&
        localStatsHaveBeenSent == true &&
        (uptimeLastMs - lastSentLocalStatsToMesh) >= Default::getLightlyScaledWindowMs(5 * 60, numOnlineNodes) &&
        airTime->isTxAllowedChannelUtil(!isImpoliteRole) && airTime->isTxAllowedAirUtil() &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN)
    {
        if(moduleConfig.node_mod_admin.local_stats_extended_over_mesh_enabled){
            sendLocalStatsExtendedToMesh();
        }

        statsHaveBeenSent = false;
        localStatsHaveBeenSent = false;
    }

    return sendToPhoneIntervalMs;
}

bool DeviceTelemetryModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    if (t->which_variant == meshtastic_Telemetry_device_metrics_tag) {
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
        const char *sender = getSenderShortName(mp);

        LOG_INFO("(Received from %s): air_util_tx=%f, channel_utilization=%f, battery_level=%i, voltage=%f", sender,
                 t->variant.device_metrics.air_util_tx, t->variant.device_metrics.channel_utilization,
                 t->variant.device_metrics.battery_level, t->variant.device_metrics.voltage);
#endif
        nodeDB->updateTelemetry(getFrom(&mp), *t, RX_SRC_RADIO);
    }
    return false; // Let others look at this message also if they want
}

meshtastic_MeshPacket *DeviceTelemetryModule::allocReply()
{
    if (currentRequest) {
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        meshtastic_Telemetry *decoded = NULL;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            decoded = &scratch;
        } else {
            LOG_ERROR("Error decoding DeviceTelemetry module!");
            return NULL;
        }
        // Check for a request for device metrics
        if (decoded->which_variant == meshtastic_Telemetry_device_metrics_tag) {
            LOG_INFO("Device telemetry reply to request");
            return allocDataProtobuf(getDeviceTelemetry());
        } else if (decoded->which_variant == meshtastic_Telemetry_local_stats_tag) {
            LOG_INFO("Device telemetry reply w/ LocalStats to request");
            return allocDataProtobuf(getLocalStatsTelemetry(false));
        }
    }
    return NULL;
}

meshtastic_Telemetry DeviceTelemetryModule::getDeviceTelemetry()
{
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.which_variant = meshtastic_Telemetry_device_metrics_tag;
    t.time = getTime();
    t.variant.device_metrics = meshtastic_DeviceMetrics_init_zero;
    t.variant.device_metrics.has_air_util_tx = true;
    t.variant.device_metrics.has_battery_level = true;
    t.variant.device_metrics.has_channel_utilization = true;
    t.variant.device_metrics.has_voltage = true;
    t.variant.device_metrics.has_uptime_seconds = true;

    t.variant.device_metrics.air_util_tx = airTime->utilizationTXPercent();
    t.variant.device_metrics.battery_level = (!powerStatus->getHasBattery() || powerStatus->getIsCharging())
                                                 ? MAGIC_USB_BATTERY_LEVEL
                                                 : powerStatus->getBatteryChargePercent();
    t.variant.device_metrics.channel_utilization = airTime->channelUtilizationPercent();
    t.variant.device_metrics.voltage = powerStatus->getBatteryVoltageMv() / 1000.0;
    t.variant.device_metrics.uptime_seconds = getUptimeSeconds();

    return t;
}

meshtastic_Telemetry DeviceTelemetryModule::getLocalStatsTelemetry(bool moreData)
{
    meshtastic_Telemetry telemetry = {};
    telemetry.which_variant = meshtastic_Telemetry_local_stats_tag;
    telemetry.time = getTime();

    telemetry.variant.local_stats.num_online_nodes = numOnlineNodes;
    telemetry.variant.local_stats.num_total_nodes = nodeDB->getNumMeshNodes();

    telemetry.variant.local_stats.has_uptime_seconds=true;
    telemetry.variant.local_stats.uptime_seconds = getUptimeSeconds(); //deprecated

    telemetry.variant.local_stats.has_channel_utilization=true;
    telemetry.variant.local_stats.has_air_util_tx=true;
    telemetry.variant.local_stats.channel_utilization = airTime->channelUtilizationPercent();
    telemetry.variant.local_stats.air_util_tx = airTime->utilizationTXPercent();


    if (RadioLibInterface::instance) {
        telemetry.variant.local_stats.num_packets_tx = RadioLibInterface::instance->txGood;
        telemetry.variant.local_stats.num_packets_rx = RadioLibInterface::instance->rxGood + RadioLibInterface::instance->rxBad;
        telemetry.variant.local_stats.num_packets_rx_bad = RadioLibInterface::instance->rxBad;
        telemetry.variant.local_stats.num_tx_relay = RadioLibInterface::instance->txRelay;
        telemetry.variant.local_stats.num_tx_dropped = RadioLibInterface::instance->txDrop;
    }
#ifdef ARCH_PORTDUINO
    if (SimRadio::instance) {
        telemetry.variant.local_stats.num_packets_tx = SimRadio::instance->txGood;
        telemetry.variant.local_stats.num_packets_rx = SimRadio::instance->rxGood + SimRadio::instance->rxBad;
        telemetry.variant.local_stats.num_packets_rx_bad = SimRadio::instance->rxBad;
        telemetry.variant.local_stats.num_tx_relay = SimRadio::instance->txRelay;
        telemetry.variant.local_stats.num_tx_dropped = SimRadio::instance->txDrop;
    }
#else
    telemetry.variant.local_stats.heap_total_bytes = memGet.getHeapSize();
    telemetry.variant.local_stats.heap_free_bytes = memGet.getFreeHeap();
#endif
    if (router) {
        telemetry.variant.local_stats.num_rx_dupe = router->rxDupe;
        telemetry.variant.local_stats.num_tx_relay_canceled = router->txRelayCanceled;
    }

    return telemetry;
}

meshtastic_Telemetry DeviceTelemetryModule::getLocalStatsExtendedTelemetry()
{
    meshtastic_Telemetry telemetry = {};
    telemetry.which_variant = meshtastic_Telemetry_local_stats_extended_tag;
    telemetry.time = getTime();

    telemetry.variant.local_stats_extended.has_memory_total=true;
    telemetry.variant.local_stats_extended.has_flash_total_bytes=true;
    telemetry.variant.local_stats_extended.has_flash_used_bytes=true;
    telemetry.variant.local_stats_extended.has_memory_free_cheap=true;
    telemetry.variant.local_stats_extended.has_cpu_usage_percent=true;

    telemetry.variant.local_stats_extended.memory_free_cheap = memGet.getFreeHeap();
    telemetry.variant.local_stats_extended.memory_total = memGet.getHeapSize();

    #if defined(ARCH_ESP32)
        spiLock->lock();
        telemetry.variant.local_stats_extended.flash_used_bytes = FSCom.usedBytes();
        telemetry.variant.local_stats_extended.flash_total_bytes = FSCom.totalBytes();
        spiLock->unlock();

        telemetry.variant.local_stats_extended.has_memory_psram_free=true;
        telemetry.variant.local_stats_extended.has_memory_psram_total=true;

        telemetry.variant.local_stats_extended.memory_psram_free = memGet.getFreePsram();
        telemetry.variant.local_stats_extended.memory_psram_total = memGet.getPsramSize();
    #endif

    #if defined(ARCH_NRF52)
        telemetry.variant.local_stats_extended.flash_used_bytes = calculateNRF5xUsedBytes();
        telemetry.variant.local_stats_extended.flash_total_bytes =  getNRF5xTotalBytes();
    #endif

    telemetry.variant.local_stats_extended.cpu_usage_percent = CpuHwUsagePercent;
    // packet history stats
    telemetry.variant.local_stats_extended.rx_packet_history_count=6;
    for (int i = 0; i < 6; i++) {
        telemetry.variant.local_stats_extended.rx_packet_history[i] = airTime->rxTxAllActivities[i].rxTxAll_counter;
    }

   telemetry.variant.local_stats_extended.rx_avg_60_min = airTime->rx_avg_60_min;
    return telemetry;
}

void DeviceTelemetryModule::sendLocalStatsToPhone()
{
    //fw+ guard pool exhaustion
    meshtastic_MeshPacket *p = allocDataProtobuf(getLocalStatsTelemetry(true));
    if (!p) {
        LOG_WARN("Skip sendLocalStatsToPhone: packetPool exhausted");
        return;
    }
    p->to = myNodeInfo.my_node_num;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToPhone(p);

    meshtastic_MeshPacket *p2 = allocDataProtobuf(getLocalStatsExtendedTelemetry());
    if (!p2) {
        LOG_WARN("Skip sendLocalStatsExtendedToPhone: packetPool exhausted");
        return;
    }
    p2->to = myNodeInfo.my_node_num;
    p2->decoded.want_response = false;
    p2->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToPhone(p2);
}

void DeviceTelemetryModule::sendLocalStatsToMesh()
{
    LOG_INFO("Sending local stats to mesh");
    meshtastic_Telemetry telemetry = getLocalStatsTelemetry(false);
    meshtastic_MeshPacket *p = allocDataProtobuf(telemetry);
    if (!p) {
        LOG_WARN("Skip sendLocalStatsToMesh: packetPool exhausted");
        return;
    }
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

void DeviceTelemetryModule::sendLocalStatsExtendedToMesh()
{
    LOG_INFO("Sending local stats extended to mesh");
    meshtastic_Telemetry telemetry = getLocalStatsExtendedTelemetry();
    meshtastic_MeshPacket *p = allocDataProtobuf(telemetry);
    if (!p) {
        LOG_WARN("Skip sendLocalStatsExtendedToMesh: packetPool exhausted");
        return;
    }
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

bool DeviceTelemetryModule::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    meshtastic_Telemetry telemetry = getDeviceTelemetry();
    LOG_INFO("Send: air_util_tx=%f, channel_utilization=%f, battery_level=%i, voltage=%f, uptime=%i",
             telemetry.variant.device_metrics.air_util_tx, telemetry.variant.device_metrics.channel_utilization,
             telemetry.variant.device_metrics.battery_level, telemetry.variant.device_metrics.voltage,
             telemetry.variant.device_metrics.uptime_seconds);

    DEBUG_HEAP_BEFORE;
    meshtastic_MeshPacket *p = allocDataProtobuf(telemetry);
    if (!p) {
        LOG_WARN("Skip DeviceTelemetry send: packetPool exhausted");
        return false;
    }
    DEBUG_HEAP_AFTER("DeviceTelemetryModule::sendTelemetry", p);

    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    nodeDB->updateTelemetry(nodeDB->getNodeNum(), telemetry, RX_SRC_LOCAL);
    if (phoneOnly) {
        p->to = myNodeInfo.my_node_num;
        service->sendToPhone(p);
    } else {
        service->sendToMesh(p, RX_SRC_LOCAL, true);
        statsHaveBeenSent = true;
    }

    return true;
}
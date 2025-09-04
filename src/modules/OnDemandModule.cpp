#include "OnDemandModule.h"
#include "../mesh/generated/meshtastic/ondemand.pb.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "NextHopRouter.h"
#include "configuration.h"
#include "main.h"
#include <meshUtils.h>
#include <pb_encode.h>
#include "ProtobufModule.h"
#include "SPILock.h"
#include "FSCommon.h"
 

OnDemandModule *onDemandModule;
static const int MAX_NODES_PER_PACKET = 10;
static const int MAX_PACKET_SIZE = 190;
#define NUM_ONLINE_SECS (60 * 60 * 2) 
#define MAGIC_USB_BATTERY_LEVEL 101

#define FW_PLUS_VERSION 23

int32_t OnDemandModule::runOnce()
{
    return default_broadcast_interval_secs;
}

bool OnDemandModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *t)
{
    if (t->which_variant == meshtastic_OnDemand_request_tag) 
    {
        switch(t->variant.request.request_type) 
        {
            case meshtastic_OnDemandType_REQUEST_FW_PLUS_VERSION: {
                sendPacketToRequester(prepareFwPlusVersion(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_NODE_STATS: {
                sendPacketToRequester(prepareNodeStats(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_AIR_ACTIVITY_HISTORY: {
                sendPacketToRequester(prepareAirActivityHistoryLog(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_PACKET_EXCHANGE_HISTORY: {
                sendPacketToRequester(preparePacketHistoryLog(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_PORT_COUNTER_HISTORY: {
                sendPacketToRequester(preparePortCounterHistory(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY: {
                sendPacketToRequester(prepareRxPacketHistory(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_RX_AVG_TIME: {
                sendPacketToRequester(prepareRxAvgTimeHistory(), mp);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_NODES_ONLINE: {
                auto packets = createSegmentedNodeList(false);
                for (auto &pkt : packets)
                {
                    sendPacketToRequester(*pkt, mp);
                } 
                break;
            }
            case meshtastic_OnDemandType_REQUEST_NODES_DIRECT_ONLINE: {
                auto packets = createSegmentedNodeList(true);
                for (auto &pkt : packets)
                {
                    sendPacketToRequester(*pkt, mp);
                }
                break;
            }
            case meshtastic_OnDemandType_REQUEST_PING: {
                sendPacketToRequester(preparePingResponse(mp), mp, false);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_PING_ACK: {
                sendPacketToRequester(preparePingResponseAck(mp), mp, true);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_ROUTING_ERRORS: {
                sendPacketToRequester(prepareRoutingErrorResponse(), mp, true);
                break;
            }
            case meshtastic_OnDemandType_REQUEST_ROUTING_TABLE: {
                auto packets = createSegmentedRoutingTable();
                for (auto &pkt : packets)
                {
                    sendPacketToRequester(*pkt, mp);
                }
                break;
            }
            default:
                meshtastic_OnDemand unknown_ondemand = meshtastic_OnDemand_init_zero;
                unknown_ondemand.which_variant = meshtastic_OnDemand_response_tag;
                unknown_ondemand.variant.response.response_type = meshtastic_OnDemandType_UNKNOWN_TYPE;
                sendPacketToRequester(unknown_ondemand, mp);
                break;
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

std::vector<std::unique_ptr<meshtastic_OnDemand>> OnDemandModule::createSegmentedNodeList(bool directOnly)
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
        onDemand->variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODES_ONLINE; // payload type unchanged
        onDemand->variant.response.which_response_data = meshtastic_OnDemandResponse_node_list_tag;

        onDemand->has_packet_index=true;
        onDemand->has_packet_total=true;

        onDemand->packet_index = packetIndex;
        meshtastic_NodesList &listRef = onDemand->variant.response.response_data.node_list;
        listRef.node_list_count = 0;

        while (currentIndex < totalNodes)
        {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(currentIndex);
            // filter: direct only if requested (hop==0 and not via mqtt)
            if (directOnly) {
                if (!(node->hops_away == 0 && !node->via_mqtt)) {
                    currentIndex++;
                    continue;
                }
            }

            meshtastic_NodeEntry entry = meshtastic_NodeEntry_init_zero;

             if (sinceLastSeen(node) >= NUM_ONLINE_SECS){
                currentIndex++;
                continue;
             }

             if(node->num == myNodeInfo.my_node_num){
                currentIndex++;
                continue;
             }
               
            entry.node_id = node->num;
            entry.last_heard = sinceLastSeen(node);
            entry.snr = 0;
            entry.hops = node->hops_away;

            if(node->hops_away==0){
                entry.snr = node->snr;
            }
            
            strncpy(entry.long_name, node->user.long_name, sizeof(entry.long_name) - 1);
            strncpy(entry.short_name, node->user.short_name, sizeof(entry.short_name) - 1);

            // Populate optional integer latitude/longitude if available
            if (nodeDB->hasValidPosition(node)) {
                entry.has_latitude_i = true;
                entry.latitude_i = node->position.latitude_i;
                entry.has_longitude_i = true;
                entry.longitude_i = node->position.longitude_i;
            }

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

meshtastic_OnDemand OnDemandModule::prepareRoutingErrorResponse()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_ROUTING_ERRORS;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_routing_errors_tag;

    onDemand.variant.response.response_data.routing_errors.routing_errors_count=39;
    for (uint16_t i = 0; i < 38; i++) {
            onDemand.variant.response.response_data.routing_errors.routing_errors[i].num = i;
            onDemand.variant.response.response_data.routing_errors.routing_errors[i].counter = router->packetErrorCounters[i];
    }
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePingResponse(const meshtastic_MeshPacket &mp)
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PING;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_ping_tag;

    if(mp.from != 0x0 && mp.from != nodeDB->getNodeNum()){
        int hopLimit = mp.hop_limit;
        int hopStart = mp.hop_start;

        if (hopLimit == hopStart)
        {
            onDemand.variant.response.response_data.ping.has_rx_rssi=true;
            onDemand.variant.response.response_data.ping.has_snr=true;

            onDemand.variant.response.response_data.ping.rx_rssi = mp.rx_rssi;
            onDemand.variant.response.response_data.ping.snr = mp.rx_snr;
        }
    }

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePingResponseAck(const meshtastic_MeshPacket &mp)
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PING_ACK;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_ping_tag;

    if(mp.from != 0x0 && mp.from != nodeDB->getNodeNum()){
        int hopLimit = mp.hop_limit;
        int hopStart = mp.hop_start;

        if (hopLimit == hopStart)
        {
            onDemand.variant.response.response_data.ping.has_rx_rssi = true;
            onDemand.variant.response.response_data.ping.has_snr = true;

            onDemand.variant.response.response_data.ping.rx_rssi = mp.rx_rssi;
            onDemand.variant.response.response_data.ping.snr = mp.rx_snr;
        }
    }

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRxAvgTimeHistory()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_RX_AVG_TIME;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_rx_avg_time_history_tag;
    onDemand.variant.response.response_data.rx_avg_time_history.rx_avg_history_count = 40;
 
    memcpy(onDemand.variant.response.response_data.rx_avg_time_history.rx_avg_history, airTime->rxWindowAverages, 40 * sizeof(uint32_t));

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareFwPlusVersion()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_FW_PLUS_VERSION;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_fw_plus_version_tag;
    onDemand.variant.response.response_data.fw_plus_version.version_number = FW_PLUS_VERSION;
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareNodeStats()
{   
    refreshUptime();
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODE_STATS;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_node_stats_tag;

    onDemand.variant.response.response_data.node_stats.has_battery_level = true;
    onDemand.variant.response.response_data.node_stats.has_voltage = true;
    onDemand.variant.response.response_data.node_stats.has_channel_utilization= true;
    onDemand.variant.response.response_data.node_stats.has_air_util_tx= true;
    onDemand.variant.response.response_data.node_stats.has_uptime_seconds = true;
    onDemand.variant.response.response_data.node_stats.has_num_packets_tx = true;
    onDemand.variant.response.response_data.node_stats.has_num_packets_rx= true;
    onDemand.variant.response.response_data.node_stats.has_num_packets_rx_bad = true;
    onDemand.variant.response.response_data.node_stats.has_num_online_nodes = true;
    onDemand.variant.response.response_data.node_stats.has_num_total_nodes = true;
    onDemand.variant.response.response_data.node_stats.has_num_rx_dupe = true;
    onDemand.variant.response.response_data.node_stats.has_num_tx_relay_canceled = true;
    onDemand.variant.response.response_data.node_stats.has_num_tx_relay = true;
    onDemand.variant.response.response_data.node_stats.has_reboots = true;
    onDemand.variant.response.response_data.node_stats.has_memory_free_cheap = true;
    onDemand.variant.response.response_data.node_stats.has_memory_total = true;
    onDemand.variant.response.response_data.node_stats.has_cpu_usage_percent = true;
    onDemand.variant.response.response_data.node_stats.has_flood_counter = true;
    onDemand.variant.response.response_data.node_stats.has_nexthop_counter = true;
    onDemand.variant.response.response_data.node_stats.has_firmware_version = true;
    onDemand.variant.response.response_data.node_stats.has_blocked_by_hoplimit = true;
    onDemand.variant.response.response_data.node_stats.has_firmware_version = true;

    onDemand.variant.response.response_data.node_stats.battery_level = (!powerStatus->getHasBattery() || powerStatus->getIsCharging()) ? MAGIC_USB_BATTERY_LEVEL : powerStatus->getBatteryChargePercent();
    onDemand.variant.response.response_data.node_stats.voltage = powerStatus->getBatteryVoltageMv() / 1000.0;
    onDemand.variant.response.response_data.node_stats.channel_utilization = airTime->channelUtilizationPercent();
    onDemand.variant.response.response_data.node_stats.air_util_tx = airTime->utilizationTXPercent();
    onDemand.variant.response.response_data.node_stats.uptime_seconds = getUptimeSeconds();
    onDemand.variant.response.response_data.node_stats.num_packets_tx = RadioLibInterface::instance->txGood;
    onDemand.variant.response.response_data.node_stats.num_packets_rx = RadioLibInterface::instance->rxGood + RadioLibInterface::instance->rxBad;
    onDemand.variant.response.response_data.node_stats.num_packets_rx_bad = RadioLibInterface::instance->rxBad;
    onDemand.variant.response.response_data.node_stats.num_online_nodes = nodeDB->getNumOnlineMeshNodes(true);
    onDemand.variant.response.response_data.node_stats.num_total_nodes = nodeDB->getNumMeshNodes();
    onDemand.variant.response.response_data.node_stats.num_rx_dupe = router->rxDupe;
    onDemand.variant.response.response_data.node_stats.num_tx_relay_canceled = router->txRelayCanceled;
    onDemand.variant.response.response_data.node_stats.num_tx_relay = router->txRelayCanceled;
    onDemand.variant.response.response_data.node_stats.reboots = myNodeInfo.reboot_count;
    onDemand.variant.response.response_data.node_stats.memory_free_cheap = memGet.getFreeHeap();
    onDemand.variant.response.response_data.node_stats.memory_total = memGet.getHeapSize();
    onDemand.variant.response.response_data.node_stats.cpu_usage_percent = CpuHwUsagePercent;
    onDemand.variant.response.response_data.node_stats.flood_counter = router->flood_counter;
    onDemand.variant.response.response_data.node_stats.nexthop_counter = router->nexthop_counter;
    onDemand.variant.response.response_data.node_stats.blocked_by_hoplimit = router->blocked_by_hoplimit;
    onDemand.variant.response.response_data.node_stats.fw_plus_version = FW_PLUS_VERSION;
    onDemand.variant.response.response_data.node_stats.rebroadcast_mode = config.device.rebroadcast_mode;
    strncpy(onDemand.variant.response.response_data.node_stats.firmware_version, optstr(APP_VERSION_SHORT), sizeof(onDemand.variant.response.response_data.node_stats.firmware_version));
    onDemand.variant.response.response_data.node_stats.has_opportunistic_enabled = true;
    onDemand.variant.response.response_data.node_stats.opportunistic_enabled =  router->getOpportunisticEnabled();
    onDemand.variant.response.response_data.node_stats.has_opportunistic_mode = true;
    onDemand.variant.response.response_data.node_stats.opportunistic_mode = router->getOpportunisticProfile();


    bool valid = false;
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.time = getTime();
    m.which_variant = meshtastic_Telemetry_power_metrics_tag;
    m.variant.power_metrics = meshtastic_PowerMetrics_init_zero;

    if (ina219Sensor.hasSensor())
        valid = ina219Sensor.getMetrics(&m);
    if (ina226Sensor.hasSensor())
        valid = ina226Sensor.getMetrics(&m);
    if (ina260Sensor.hasSensor())
        valid = ina260Sensor.getMetrics(&m);
    if (ina3221Sensor.hasSensor())
        valid = ina3221Sensor.getMetrics(&m);
    if (max17048Sensor.hasSensor())
        valid = max17048Sensor.getMetrics(&m);

    if(valid){
        if(m.variant.power_metrics.ch1_voltage != 0){
            onDemand.variant.response.response_data.node_stats.has_ch1_voltage = true;
            onDemand.variant.response.response_data.node_stats.has_ch1_current = true;
            onDemand.variant.response.response_data.node_stats.ch1_voltage = m.variant.power_metrics.ch1_voltage;
            onDemand.variant.response.response_data.node_stats.ch1_current = m.variant.power_metrics.ch1_current;
        }
    
        if(m.variant.power_metrics.ch2_voltage != 0){
            onDemand.variant.response.response_data.node_stats.has_ch2_voltage = true;
            onDemand.variant.response.response_data.node_stats.has_ch2_current = true;
            onDemand.variant.response.response_data.node_stats.ch2_voltage = m.variant.power_metrics.ch2_voltage;
            onDemand.variant.response.response_data.node_stats.ch2_current = m.variant.power_metrics.ch2_current;
        }
    
        if(m.variant.power_metrics.ch3_voltage != 0){
            onDemand.variant.response.response_data.node_stats.has_ch3_voltage = true;
            onDemand.variant.response.response_data.node_stats.has_ch3_current = true;
            onDemand.variant.response.response_data.node_stats.ch3_voltage = m.variant.power_metrics.ch3_voltage;
            onDemand.variant.response.response_data.node_stats.ch3_current = m.variant.power_metrics.ch3_current;
        }  
    }

#if defined(ARCH_ESP32)
    onDemand.variant.response.response_data.node_stats.has_flash_used_bytes = true;
    onDemand.variant.response.response_data.node_stats.has_flash_total_bytes = true;
    onDemand.variant.response.response_data.node_stats.has_memory_psram_free = true;
    onDemand.variant.response.response_data.node_stats.has_memory_psram_total = true;

    spiLock->lock();
    onDemand.variant.response.response_data.node_stats.flash_used_bytes = FSCom.usedBytes();
    onDemand.variant.response.response_data.node_stats.flash_total_bytes = FSCom.totalBytes(); 
    spiLock->unlock();

    onDemand.variant.response.response_data.node_stats.memory_psram_free = memGet.getFreePsram();
    onDemand.variant.response.response_data.node_stats.memory_psram_total = memGet.getPsramSize();     
#endif

#if defined(ARCH_NRF52)
    onDemand.variant.response.response_data.node_stats.has_flash_used_bytes = true;
    onDemand.variant.response.response_data.node_stats.has_flash_total_bytes = true;
    onDemand.variant.response.response_data.node_stats.flash_used_bytes = calculateNRF5xUsedBytes(); 
    onDemand.variant.response.response_data.node_stats.flash_total_bytes =  getNRF5xTotalBytes(); 
#endif
        
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareAirActivityHistoryLog()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_AIR_ACTIVITY_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_air_activity_history_tag;

    onDemand.variant.response.response_data.air_activity_history.air_activity_history_count=10;
    for (uint16_t i = 0; i < 10; i++) {
            onDemand.variant.response.response_data.air_activity_history.air_activity_history[i].rx_time = airTime->activityWindow[i].rx_time;
            onDemand.variant.response.response_data.air_activity_history.air_activity_history[i].tx_time = airTime->activityWindow[i].tx_time;
            onDemand.variant.response.response_data.air_activity_history.air_activity_history[i].rxBad_time = airTime->activityWindow[i].rx_bad_time;
    }
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePacketHistoryLog()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PACKET_EXCHANGE_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_exchange_packet_log_tag;

    onDemand.variant.response.response_data.exchange_packet_log.exchange_list_count=12;
    for (uint16_t i = 0; i < 12; i++) {
            onDemand.variant.response.response_data.exchange_packet_log.exchange_list[i].port_num = nodeDB->packetHistoryLog.entries[i].port_num;
            onDemand.variant.response.response_data.exchange_packet_log.exchange_list[i].from_node = nodeDB->packetHistoryLog.entries[i].from_node;
            onDemand.variant.response.response_data.exchange_packet_log.exchange_list[i].to_node = nodeDB->packetHistoryLog.entries[i].to_node;   
    }
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePortCounterHistory()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PORT_COUNTER_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_port_counter_history_tag;

    uint8_t entryCount = 0;
    for (uint16_t i = 0; i < MAX_PORTS && entryCount < 20; i++) {
        if (portCounters[i] > 0) 
        {
            onDemand.variant.response.response_data.port_counter_history.port_counter_history[entryCount].port = i;
            onDemand.variant.response.response_data.port_counter_history.port_counter_history[entryCount].count = portCounters[i];
            entryCount++;
        }
    }
 
    onDemand.variant.response.response_data.port_counter_history.port_counter_history_count = entryCount;

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRxPacketHistory()
{   
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;

    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PACKET_RX_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_rx_packet_history_tag;
    onDemand.variant.response.response_data.rx_packet_history.rx_packet_history_count = RXTXALL_ACTIVITY_COUNT;
 
    memcpy(onDemand.variant.response.response_data.rx_packet_history.rx_packet_history, airTime->rxTxAllActivities, RXTXALL_ACTIVITY_COUNT * sizeof(uint32_t));

    return onDemand;
}

void OnDemandModule::sendPacketToRequester(const meshtastic_OnDemand &demand_packet,const meshtastic_MeshPacket &mp, bool wantAck){

    meshtastic_MeshPacket *p = allocDataProtobuf(demand_packet);
    p->to = mp.from;
    p->decoded.want_response = false;
    p->want_ack = wantAck;
    p->channel = mp.channel;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
 
    if(mp.from == RX_SRC_LOCAL){ 
        p->to = myNodeInfo.my_node_num;
        service->sendToPhone(p);
    }else{
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
}

std::vector<std::unique_ptr<meshtastic_OnDemand>> OnDemandModule::createSegmentedRoutingTable()
{
    std::vector<std::unique_ptr<meshtastic_OnDemand>> packets;

    if (!router)
        return packets;
    auto nh = static_cast<NextHopRouter *>(router);

    auto snapshot = nh->getRouteSnapshot(false);
    int currentIndex = 0;
    int packetIndex = 1;

    while (currentIndex < (int)snapshot.size())
    {
        std::unique_ptr<meshtastic_OnDemand> onDemand(new meshtastic_OnDemand);
        *onDemand = meshtastic_OnDemand_init_zero;

        onDemand->which_variant = meshtastic_OnDemand_response_tag;
        onDemand->variant.response.response_type = meshtastic_OnDemandType_RESPONSE_ROUTING_TABLE;
        onDemand->variant.response.which_response_data = meshtastic_OnDemandResponse_routing_table_tag;

        onDemand->has_packet_index = true;
        onDemand->has_packet_total = true;
        onDemand->packet_index = packetIndex;

        meshtastic_RoutingTable &listRef = onDemand->variant.response.response_data.routing_table;
        listRef.routes_count = 0;

        while (currentIndex < (int)snapshot.size())
        {
            const auto &e = snapshot[currentIndex];
            meshtastic_RoutingTableEntry entry = meshtastic_RoutingTableEntry_init_zero;
            entry.dest = e.dest;
            entry.next_hop = e.next_hop;
            entry.cost = e.aggregated_cost;
            entry.confidence = e.confidence;
            uint32_t now = millis();
            entry.age_secs = (now >= e.lastUpdatedMs) ? (now - e.lastUpdatedMs) / 1000 : 0;

            int pos = listRef.routes_count;
            listRef.routes[pos] = entry;
            listRef.routes_count++;

            if (!fitsInPacket(*onDemand, MAX_PACKET_SIZE))
            {
                listRef.routes_count--;
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

    if (packets.empty())
    {
        std::unique_ptr<meshtastic_OnDemand> onDemand(new meshtastic_OnDemand);
        *onDemand = meshtastic_OnDemand_init_zero;
        onDemand->which_variant = meshtastic_OnDemand_response_tag;
        onDemand->variant.response.response_type = meshtastic_OnDemandType_RESPONSE_ROUTING_TABLE;
        onDemand->variant.response.which_response_data = meshtastic_OnDemandResponse_routing_table_tag;
        onDemand->has_packet_index = true;
        onDemand->has_packet_total = true;
        onDemand->packet_index = 1;
        onDemand->packet_total = 1;
        packets.push_back(std::move(onDemand));
    }

    return packets;
}

meshtastic_MeshPacket *OnDemandModule::allocReply()
{
    return NULL;
}
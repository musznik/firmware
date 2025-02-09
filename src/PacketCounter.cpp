#include "PacketCounter.h"
#include "NodeDB.h"

void PacketCounter::onPacketReceived(const meshtastic_MeshPacket *p)
{
    //echange packet history 
    nodeDB->packetHistoryLog.addEntry({p->from, p->to, p->decoded.portnum});

    //counters rxhistory
    uint64_t max_entries = 39;
    currentBucketCount++;
    uint64_t nowMs = getMonotonicUptimeMs();
    if (nowMs - bucketStartMs >= 600000ULL) { // 600000 ms = 10 min
        for (int i = max_entries; i > 0; i--) {
            moduleConfig.nodemodadmin.rx_packet_history[i] =
                moduleConfig.nodemodadmin.rx_packet_history[i - 1];
        }

        moduleConfig.nodemodadmin.rx_packet_history[0] = currentBucketCount;

        if (moduleConfig.nodemodadmin.rx_packet_history_count < max_entries) {
            moduleConfig.nodemodadmin.rx_packet_history_count++;
        }

        currentBucketCount = 0;
        bucketStartMs = nowMs;
    }

    moduleConfig.nodemodadmin.rx_avg_60_min = getAvgLast60Min();
}

uint64_t PacketCounter::getMonotonicUptimeMs()
{
    static uint64_t extendedMs = 0;
    static uint32_t lastMs = 0;

    uint32_t nowMs = millis();
    int32_t diff = (int32_t)(nowMs - lastMs);

    lastMs = nowMs;   

    // diff < 0, counter wrap
    if (diff < 0) {
        diff += 0x100000000ULL;  // add 2^32
    }

    extendedMs += diff;
    return extendedMs;
}

uint32_t PacketCounter::getAvgLast60Min()
{
    int count = moduleConfig.nodemodadmin.rx_packet_history_count;
    if (count == 0) {
        return 0; 
    }

    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += moduleConfig.nodemodadmin.rx_packet_history[i];
    }
    uint32_t avg = sum / count;
    return avg;
}
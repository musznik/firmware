#include "PacketCounter.h"
#include "NodeDB.h"
#include "airtime.h"

// Called for overheard RX (Router::sniffReceived) and for our TX (Router::send); bucket totals are RX+TX mesh packets.
void PacketCounter::onPacketReceived(const meshtastic_MeshPacket *p)
{
    // log packet history
    if(p->pki_encrypted){
        meshtastic_PortNum portnum = meshtastic_PortNum_UNKNOWN_APP;
        nodeDB->packetHistoryLog.addEntry({p->from, p->to, portnum});
    }else{
        nodeDB->packetHistoryLog.addEntry({p->from, p->to, p->decoded.portnum});    
    }
 
    currentBucketCount++;
    uint64_t nowMs = getMonotonicUptimeMs();
    if (nowMs - bucketStartMs >= 600000ULL) { // 10 min bucket rollover
        const int lastIdx = RXTXALL_ACTIVITY_COUNT - 1;
        for (int i = lastIdx; i > 0; i--) {
            airTime->rxTxAllActivities[i] = airTime->rxTxAllActivities[i - 1];
        }

        airTime->rxTxAllActivities[0].rxTxAll_counter = currentBucketCount;

        if (airTime->rxTxAllActivitiesCount < RXTXALL_ACTIVITY_COUNT) {
            airTime->rxTxAllActivitiesCount++;
        }

        currentBucketCount = 0;
        bucketStartMs = nowMs;
    }

    airTime->rxPacketBucketPartial = currentBucketCount;
    airTime->rx_avg_60_min = getAvgLast60Min();
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
    int count = airTime->rxTxAllActivitiesCount;
    if (count == 0) {
        return 0; 
    }

    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += airTime->rxTxAllActivities[i].rxTxAll_counter;
    }
    uint32_t avg = sum / count;
    return avg;
}
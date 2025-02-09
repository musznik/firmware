#pragma once
#include <stdint.h>
#include <stddef.h>
#include <deque>
// #include "MeshService.h"
#include "NodeDB.h"
/**
 * Klasa zliczająca pakiety w "ruchomym oknie" 10 minut.
 * Za każdym razem, gdy przychodzi nowy pakiet, wywołujemy onPacketReceived().
 * Aby odczytać liczbę pakietów z ostatnich 10 min, wywołaj getCountLast10Min().
 */
class PacketCounter
{
public:
    PacketCounter() = default;

    int currentBucketIndex = 0;
    uint64_t bucketStartMs = 0;
    uint32_t currentBucketCount = 0;

    void onPacketReceived(const meshtastic_MeshPacket *p);
    void addRxPacketHistory(meshtastic_ModuleConfig_NodeModAdminConfig &admin, uint64_t timestamp);
    uint32_t getAvgLast60Min();
    size_t getCountLast10Min(uint64_t currentUptimeMs) const;

private:
    uint64_t getMonotonicUptimeMs();
    std::deque<uint64_t> m_timestamps;
};

 
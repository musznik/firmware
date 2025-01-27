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

    /**
     * Zgłoś, że odebrano nowy pakiet w momencie @param currentUptimeMs (ms od startu urządzenia)
     */
    void onPacketReceived();
    void addRxPacketHistory(meshtastic_ModuleConfig_NodeModAdminConfig &admin, uint64_t timestamp);
    uint32_t getAvgLast60Min();

    /**
     * Zwraca ile pakietów było w ciągu ostatnich 10 minut (600000 ms),
     * względem @param currentUptimeMs.
     */
    size_t getCountLast10Min(uint64_t currentUptimeMs) const;

private:
    uint64_t getMonotonicUptimeMs();
    // Kolejka przechowująca timestampy w milisekundach
    std::deque<uint64_t> m_timestamps;
};

 
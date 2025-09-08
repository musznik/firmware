#pragma once

#include "ProtobufModule.h" //fw+
#include "mesh/generated/meshtastic/heard.pb.h" //fw+
#include <unordered_map>
#include <vector>

//fw+ HeardModule: sampled HEARD reports for text broadcast coverage (scaffold)
class HeardModule : public ProtobufModule<meshtastic_HeardReport> //fw+
{
  public:
    //fw+
    HeardModule();

    //fw+
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_HeardReport *p) override { return false; }

    //fw+
    virtual void alterReceivedProtobuf(meshtastic_MeshPacket &p, meshtastic_HeardReport *decoded) override;

    //fw+
    virtual int32_t runOnce();

  private:
    //fw+ original broadcast identity key
    struct OriginalKey {
        uint32_t src;
        uint32_t pkt;
        bool operator==(const OriginalKey &o) const { return src == o.src && pkt == o.pkt; }
    };

    //fw+ hash for OriginalKey
    struct OriginalKeyHash {
        size_t operator()(const OriginalKey &k) const { return (std::hash<uint32_t>()(k.src) << 1) ^ std::hash<uint32_t>()(k.pkt); }
    };

    //fw+ lightweight report buffer (independent of protobuf)
    struct ReportLite {
        uint32_t receiver;
        int32_t rx_rssi;
        float rx_snr;
        uint32_t hop_count;
        uint32_t rx_ms;
    };

    //fw+ aggregation state per original packet
    struct AggregationState {
        std::vector<ReportLite> reports;
        uint32_t expireMs = 0;
    };

    //fw+
    std::unordered_map<OriginalKey, AggregationState, OriginalKeyHash> aggregates;

    //fw+ params (defaults; can be later sourced from nodemodadmin)
    uint8_t samplePercentTexts = 0;     // 0 = disabled by default
    uint16_t windowMs = 1000;           // collection window
    uint8_t maxReportsPerAgg = 12;      // cap per aggregate
    uint16_t maxActiveKeys = 64;        // cap active aggregates

    //fw+ helpers
    bool shouldSampleTextBroadcast(const meshtastic_MeshPacket *p) const;
    void maybeScheduleHeard(const meshtastic_MeshPacket *p);
    void ingestAndMaybeAggregate(const meshtastic_MeshPacket *p);
    void flushAggregate(const OriginalKey &k);
    uint32_t computeHopCount(const meshtastic_MeshPacket *p) const;
    uint32_t nowMs() const;

    //fw+ assist decision inputs (simple cache for last text broadcast seen in cluster)
    struct TextCacheEntry {
        uint32_t packet_id = 0;
        uint32_t first_seen_ms = 0;
        uint8_t rebroadcasts = 0;
        // We do not cache payload bytes here to avoid memory overhead; assist reuses control-only
    };
    TextCacheEntry lastText;

    void maybeDecideAssist(const OriginalKey &k);
    void sendAssistControl(uint32_t ref_packet_id);
};



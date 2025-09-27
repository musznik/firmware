#pragma once

#include "MeshModule.h"
#include "NodeDB.h"
#include "MeshService.h"
#include "airtime.h"

//fw+ BroadcastAssist: selective reflooding of broadcasts in sparse neighborhoods
struct BaStatsSnapshot {
    bool enabled = false;
    uint32_t refloodAttempts = 0;
    uint32_t refloodSent = 0;
    uint32_t suppressedDup = 0;
    uint32_t suppressedDegree = 0;
    uint32_t suppressedAirtime = 0;
    uint32_t lastRefloodAgeSecs = 0;
    //fw+ upstream router duplicate drops observed in this node
    uint32_t upstreamDupDropped = 0;
};

extern class BroadcastAssistModule *broadcastAssistModule;
class BroadcastAssistModule : public MeshModule
{
  public:
    BroadcastAssistModule();
    //fw+ observe when router drops a duplicate upstream (pre-module)
    inline void onUpstreamDupeDropped() { statUpstreamDupDropped++; }

  protected:
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    struct SeenRec {
        uint32_t id = 0;
        uint32_t firstMs = 0;
        uint16_t count = 0;
        bool reflooded = false;
    };

    static const int SEEN_CAP = 32;
    SeenRec seen[SEEN_CAP];
    int seenIdx = 0;

    // helpers
    SeenRec *findOrCreate(uint32_t id, uint32_t nowMs);
    uint8_t countDirectNeighbors(uint32_t freshnessSecs = 3600) const;
    bool isAllowedPort(const meshtastic_MeshPacket &mp) const;
    bool airtimeOk() const;
    float computeRefloodProbability(uint8_t neighborCount) const;

    // stats
    uint32_t statRefloodAttempts = 0;
    uint32_t statRefloodSent = 0;
    uint32_t statSuppressedDup = 0;
    uint32_t statSuppressedDegree = 0;
    uint32_t statSuppressedAirtime = 0;
    uint32_t lastRefloodMs = 0;
    //fw+ count of upstream dedup drops signaled by router
    uint32_t statUpstreamDupDropped = 0;

  public:
    void getStatsSnapshot(BaStatsSnapshot &out) const;
};



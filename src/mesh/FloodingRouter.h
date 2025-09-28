#pragma once

#include "Router.h"

/**
 * This is a mixin that extends Router with the ability to do Naive Flooding (in the standard mesh protocol sense)
 *
 *   Rules for broadcasting (listing here for now, will move elsewhere eventually):

  If to==BROADCAST and id==0, this is a simple broadcast (0 hops).  It will be
  sent only by the current node and other nodes will not attempt to rebroadcast
  it.

  If to==BROADCAST and id!=0, this is a "naive flooding" broadcast.  The initial
  node will send it on all local interfaces.

  When other nodes receive this message, they will
  first check if their recentBroadcasts table contains the (from, id) pair that
  indicates this message.  If so, we've already seen it - so we discard it.  If
  not, we add it to the table and then resend this message on all interfaces.
  When resending we are careful to use the "from" ID of the original sender. Not
  our own ID.  When resending we pick a random delay between 0 and 10 seconds to
  decrease the chance of collisions with transmitters we can not even hear.

  Any entries in recentBroadcasts that are older than X seconds (longer than the
  max time a flood can take) will be discarded.
 */
class FloodingRouter : public Router
{
  private:
    /* Check if we should rebroadcast this packet, and do so if needed */
    void perhapsRebroadcast(const meshtastic_MeshPacket *p);

    // Telemetry rebroadcast limiter state (60s fixed window)
    uint32_t telemetryWindowStartMs = 0;
    uint16_t telemetryPacketsInWindow = 0;
    bool isTelemetryRebroadcastLimited(const meshtastic_MeshPacket *p);

    // Position rebroadcast limiter - cache of last forwarded positions (LRU, fixed size)
    struct ForwardedPositionEntry {
      uint32_t nodeId = 0;
      int32_t lastLat_i = 0;
      int32_t lastLon_i = 0;
      uint32_t lastRebroadcastMs = 0;
    };
    static constexpr size_t kMaxPositionEntries = 64;
    ForwardedPositionEntry recentForwardedPositions[kMaxPositionEntries] = {};
    size_t recentForwardedPositionsCount = 0;
    bool isPositionRebroadcastAllowed(const meshtastic_MeshPacket *p);
    void upsertPositionEntryLRU(uint32_t nodeId, int32_t lat_i, int32_t lon_i, uint32_t nowMs);

    // Opportunistic/selective flooding helpers
    bool isOpportunisticEnabled() const;
    bool isOpportunisticAuto() const;
    uint32_t computeOpportunisticDelayMs(const meshtastic_MeshPacket *p) const;
    uint32_t clampDelay(uint32_t d) const;
    bool hasBackboneNeighbor() const;

    // Counters for diagnostics
    uint32_t opportunistic_scheduled = 0;
    uint32_t opportunistic_canceled = 0;

  public:
    /**
     * Constructor
     *
     */
    FloodingRouter();

    // Opportunistic profile categories
    enum class OpportunisticProfile : uint8_t { SPARSE = 0, BALANCED = 1, DENSE = 2, BACKBONE_BRIDGE = 3 };

    struct ProfileParams {
      uint16_t base = 60;
      uint16_t hop = 30;
      uint8_t snrGain = 8;
      uint16_t jitter = 40;
      uint8_t backboneBias = 0; // extra earlier ms if backbone
      // Future: cancelDeferMs, cancelMinDupes
    };

  private:
    // Adaptive profile state (windowed)
    OpportunisticProfile currentProfile = OpportunisticProfile::BALANCED;
    OpportunisticProfile targetProfile = OpportunisticProfile::BALANCED;
    uint32_t profileWindowStartMs = 0;
    uint32_t profileWindowMs = 60000; // 60s
    uint8_t stableWindows = 0;
    uint8_t requiredStableWindows = 2;

    // Window metrics
    uint16_t windowBroadcastNonDup = 0; // non-duplicate broadcasts seen in window
    uint32_t lastRxDupeCounter = 0;     // snapshot of Router::rxDupe at window start
    uint8_t neighborsWin = 0;           // estimated local neighbor count
    float chanUtilEma = 0.0f;           // EMA of channel utilization

    // Per-profile params (can be tuned later) - set in constructor
    ProfileParams profileParamsSparse;
    ProfileParams profileParamsBalanced;
    ProfileParams profileParamsDense;
    ProfileParams profileParamsBridge;

    // Adaptive helpers
    void observeRxForProfile(const meshtastic_MeshPacket *p);
    void maybeRecomputeProfile(uint32_t nowMs);
    const ProfileParams &getParamsFor(OpportunisticProfile p) const;

  protected:
    /**
     * Send a packet on a suitable interface.  This routine will
     * later free() the packet to pool.  This routine is not allowed to stall.
     * If the txmit queue is full it might return an error
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;

    // Expose opportunistic info for OnDemand
    virtual uint32_t getOpportunisticProfile() const override { return (uint32_t)currentProfile; }
    virtual bool getOpportunisticEnabled() const override { return isOpportunisticEnabled(); }
    /**
     * Should this incoming filter be dropped?
     *
     * Called immediately on reception, before any further processing.
     * @return true to abandon the packet
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) override;

    /**
     * Look for broadcasts we need to rebroadcast
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c) override;

    // Return false for roles like ROUTER which should always rebroadcast even when we've heard another rebroadcast of
    // the same packet
    bool roleAllowsCancelingDupe(const meshtastic_MeshPacket *p);

    /* Call when receiving a duplicate packet to check whether we should cancel a packet in the Tx queue */
    void perhapsCancelDupe(const meshtastic_MeshPacket *p);

    // Return true if we are a rebroadcaster
    bool isRebroadcaster();
};
#pragma once

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/storeforward.pb.h"

#include "configuration.h"
#include <Arduino.h>
#include <functional>
#include <unordered_map>
#include <unordered_set> //fw+
#include <vector>

struct PacketHistoryStruct {
    uint32_t time;
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t channel;
    uint32_t reply_id;
    bool emoji;
    //fw+ mark whether payload[] holds encrypted bytes (opaque custody)
    bool encrypted;
    uint8_t payload[meshtastic_Constants_DATA_PAYLOAD_LEN];
    pb_size_t payload_size;
    int32_t rx_rssi;
    float rx_snr;
};

class StoreForwardModule : private concurrency::OSThread, public ProtobufModule<meshtastic_StoreAndForward>
{
    bool busy = 0;
    uint32_t busyTo = 0;
    char routerMessage[meshtastic_Constants_DATA_PAYLOAD_LEN] = {0};

    PacketHistoryStruct *packetHistory = 0;
    uint32_t packetHistoryTotalCount = 0;
    uint32_t last_time = 0;
    uint32_t requestCount = 0;

    uint32_t packetTimeMax = 5000; // Interval between sending history packets as a server.

    bool is_client = false;
    bool is_server = false;

    // Unordered_map stores the last request for each nodeNum (`to` field)
    std::unordered_map<NodeNum, uint32_t> lastRequest;

    //fw+ S&F custody scheduling (server-side)
    struct CustodySchedule {
        uint32_t id;
        uint32_t to;
        bool isDM;
        uint32_t nextAttemptMs;
        uint8_t tries;
        //fw+ track last transmission time to avoid overlapping with router retransmissions
        uint32_t lastTxMs = 0;
        //fw+ overall deadline to give up (since custody start)
        uint32_t deadlineMs = 0;
    };
    std::unordered_map<uint32_t, CustodySchedule> scheduleById; // id -> schedule
    std::unordered_map<uint32_t, bool> pendingSchedule;         // ids awaiting history entry
    std::unordered_set<uint32_t> deliveredIds;                  //fw+ delivered DMs
    std::unordered_set<uint32_t> claimedIds;                    //fw+ custody-claimed elsewhere
    std::unordered_map<uint32_t, uint32_t> forwardedToOriginal; //fw+ forwardedId -> originalId

    // Scheduling parameters (can be tuned later / moved to ModuleConfig)
    uint32_t dmInitialBaseMs = 5000;       // base 5s (legacy; initial now uses computeInitialDelayMs)
    float dmHopCoefMs = 400.0f;            // ~0.4s per hop; doubled to ~0.8s RT budget
    float dmBackoffFactor = 1.8f;          // legacy exponential (not used for DM retries)
    uint32_t dmMaxDelayMs = 5 * 60 * 1000; // fw+ overall cap 5 minutes
    uint8_t dmMaxTries = 3;                //fw+ reduce retries to limit airtime spam
    uint8_t dmJitterPct = 15;              // +/- percent
    uint32_t bcMinDelayMs = 6000;          // 6s
    uint32_t bcMaxDelayMs = 12000;         // 12s
    uint8_t bcJitterPct = 20;
    uint32_t busyRetryMs = 2500;           //fw+ retry when channel busy
    //fw+ enforce minimum spacing between S&F retries to prevent overlap; now 60s base pacing
    uint32_t minRetrySpacingMs = 60000;    // 60s

  public:
    //fw+ accept encrypted packets for opaque custody
    bool getEncryptedOk() const { return true; }
    StoreForwardModule();

    unsigned long lastHeartbeat = 0;
    uint32_t heartbeatInterval = 900;

    /**
     Update our local reference of when we last saw that node.
     @return 0 if we have never seen that node before otherwise return the last time we saw the node.
     */
    void historyAdd(const meshtastic_MeshPacket &mp);
    void statsSend(uint32_t to);
    void historySend(uint32_t secAgo, uint32_t to);
    uint32_t getNumAvailablePackets(NodeNum dest, uint32_t last_time);

    /**
     * Send our payload into the mesh
     */
    bool sendPayload(NodeNum dest = NODENUM_BROADCAST, uint32_t packetHistory_index = 0);
    meshtastic_MeshPacket *preparePayload(NodeNum dest, uint32_t packetHistory_index, bool local = false);
    void sendMessage(NodeNum dest, const meshtastic_StoreAndForward &payload);
    void sendMessage(NodeNum dest, meshtastic_StoreAndForward_RequestResponse rr);
    void sendErrorTextMessage(NodeNum dest, bool want_response);
    meshtastic_MeshPacket *getForPhone();
    // Returns true if we are configured as server AND we could allocate PSRAM.
    bool isServer() { return is_server; }

    //fw+ Cancel custody schedule for given original message id (on observed ACK)
    void cancelScheduleForId(uint32_t id) { scheduleById.erase(id); }
    //fw+ Cancel only if ACK came from the intended DM recipient
    void cancelScheduleOnAck(uint32_t id, NodeNum ackFrom);
    //fw+ Mark an id delivered and neutralize any history records
    void markDelivered(uint32_t id);
    bool isDelivered(uint32_t id) const { return deliveredIds.find(id) != deliveredIds.end(); }
    //fw+ mark/verify external custody claim
    void markClaimed(uint32_t id) { claimedIds.insert(id); }
    bool isClaimed(uint32_t id) const { return claimedIds.find(id) != claimedIds.end(); }
    //fw+ mapping helpers for forwarded DM ids
    void rememberForwarded(uint32_t forwardedId, uint32_t originalId) { forwardedToOriginal[forwardedId] = originalId; }
    uint32_t translateForwardedToOriginal(uint32_t id) const
    {
        auto it = forwardedToOriginal.find(id);
        return it == forwardedToOriginal.end() ? 0u : it->second;
    }
    void forgetForwarded(uint32_t forwardedId) { forwardedToOriginal.erase(forwardedId); }

    //fw+ S&F status helpers for OnDemand
    uint32_t getActiveDmCount() const
    {
        uint32_t count = 0;
        for (const auto &kv : scheduleById) if (kv.second.isDM) count++;
        return count;
    }
    uint32_t getActiveBroadcastCount() const
    {
        uint32_t count = 0;
        for (const auto &kv : scheduleById) if (!kv.second.isDM) count++;
        return count;
    }
    uint32_t getDeliveredTotalCount() const { return (uint32_t)deliveredIds.size(); }
    uint32_t getClaimedTotalCount() const { return (uint32_t)claimedIds.size(); }
    uint8_t getDmMaxTries() const { return dmMaxTries; }
    float getDmBackoffFactor() const { return dmBackoffFactor; }
    uint32_t getMinRetrySpacingMs() const { return minRetrySpacingMs; }
    uint32_t getBusyRetryMs() const { return busyRetryMs; }
    uint32_t getHeartbeatInterval() const { return heartbeatInterval; }

    /*
      -Override the wantPacket method.
    */
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        //fw+ Opaque custody: accept encrypted DMs for server-only capture
        if (is_server && p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag && !isBroadcast(p->to)) {
            return true;
        }
        switch (p->decoded.portnum) {
        case meshtastic_PortNum_TEXT_MESSAGE_APP:
        case meshtastic_PortNum_STORE_FORWARD_APP:
            return true;
        default:
            return false;
        }
    }

  private:
    void populatePSRAM();

    // S&F Defaults
    uint32_t historyReturnMax = 25;     // Return maximum of 25 records by default.
    uint32_t historyReturnWindow = 240; // Return history of last 4 hours by default.
    uint32_t records = 0;               // Calculated
    bool heartbeat = false;             // No heartbeat.

    // stats
    uint32_t requests = 0;         // Number of times any client sent a request to the S&F.
    uint32_t requests_history = 0; // Number of times the history was requested.

    uint32_t retry_delay = 0; // If server is busy, retry after this delay (in ms).

  public:
    //fw+ expose S&F server active state to allow RAM-aware services (e.g., WebServer) to adapt
    bool isStoreForwardServerActive() const { return is_server; }
    //fw+ public wrapper for emitting delivered control (keeps core method non-public)
    void broadcastDeliveredControl(uint32_t origId) { sendCustodyDelivered(origId); }
    //fw+ public wrapper for emitting delivery-failed control
    void broadcastDeliveryFailedControl(uint32_t origId, uint32_t reasonCode) { sendDeliveryFailed(origId, reasonCode); }

  private:
    //fw+ Mini-server mode for boards without PSRAM: use tiny DRAM buffer and stricter limits
    bool miniServerMode = false; //fw+

  protected:
    virtual int32_t runOnce() override;

    /** Called to handle a particular incoming message

    @return ProcessMessage::STOP if you've guaranteed you've handled this message and no other handlers should be considered for
    it
    */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_StoreAndForward *p);

    //fw+ helpers
    void scheduleCustodyIfReady(uint32_t id);
    void scheduleFromHistory(uint32_t id);
    void processSchedules();
    uint32_t addJitter(uint32_t ms, uint8_t pct);
    uint32_t nowMs() const { return millis(); }
    //fw+ Send Custody ACK to original sender for DM takeover
    void sendCustodyAck(NodeNum to, uint32_t origId);
    //fw+ Opaque custody: add encrypted packet to history
    void historyAddOpaque(const meshtastic_MeshPacket &mp);
    //fw+ Reschedule when channel is busy
    void rescheduleAfterBusy(CustodySchedule &s);
    //fw+ helper to neutralize history entries for id; returns how many entries were cleared
    uint32_t clearHistoryById(uint32_t id);
    //fw+ count active (non-cleared) history entries
    uint32_t countActiveHistory() const;
    //fw+ custody signals
    void sendCustodyClaim(uint32_t origId);
    void sendCustodyDelivered(uint32_t origId);
    void sendDeliveryFailed(uint32_t origId, uint32_t reasonCode);
    //fw+ helper: locate last history record for id and extract endpoints
    bool getHistoryEndpoints(uint32_t id, NodeNum &src, NodeNum &dst, uint8_t &channel);
    //fw+ estimate hop distance to destination using NodeDB, fallback to default (8)
    uint8_t estimateHops(NodeNum to) const;
    //fw+ compute DM initial delay (10–30s window scaled by hops)
    uint32_t computeInitialDelayMs(uint8_t estHops) const;
    //fw+ compute DM retry target time (>=60s + hop scaling, with jitter and spacing guards)
    uint32_t computeRetryDelayMs(uint8_t tries, uint8_t estHops, uint32_t lastTxMs, uint32_t now) const;
    //fw+ dense auto-detect using NodeDB and channel utilization
    bool isDenseEnvironment() const;
    //fw+ user-visible notifications removed
};

extern StoreForwardModule *storeForwardModule;

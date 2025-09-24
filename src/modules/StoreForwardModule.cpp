/**
 * @file StoreForwardModule.cpp
 * @brief Implementation of the StoreForwardModule class.
 *
 * This file contains the implementation of the StoreForwardModule class, which is responsible for managing the store and forward
 * functionality of the Meshtastic device. The class provides methods for sending and receiving messages, as well as managing the
 * message history queue. It also initializes and manages the data structures used for storing the message history.
 *
 * The StoreForwardModule class is used by the MeshService class to provide store and forward functionality to the Meshtastic
 * device.
 *
 * @author Jm Casler
 * @date [Insert Date]
 */
#include "StoreForwardModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include "mesh/NextHopRouter.h" //fw+ DV-ETX adaptation hooks
#include "Default.h" //fw+
#include "Throttle.h"
#include "airtime.h"
#include "configuration.h"
#include "memGet.h"
#include "mesh-pb-constants.h"
#include "mesh/generated/meshtastic/storeforward.pb.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
//fw+ Minimal private custody control frame over PRIVATE_APP to keep apps from rendering S&F controls
//fw+ Use standard StoreAndForward protobuf envelope on a private custody port; no custom payload tag
static size_t fwplus_encode_private_control(uint8_t *out, size_t outLen,
                                            meshtastic_StoreAndForward_RequestResponse rr,
                                            uint32_t id, uint32_t reason)
{
    // Deprecated: retained for compatibility; no longer used
    return 0;
}

void StoreForwardModule::sendCustodyControlPrivate(NodeNum dest, meshtastic_StoreAndForward_RequestResponse rr,
                                                   uint32_t origId, uint32_t reasonCode)
{
    //fw+ Send a normal StoreAndForward protobuf on a private port so apps ignore it
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero; //fw+
    sf.rr = rr;                                                          //fw+
    sf.which_variant = meshtastic_StoreAndForward_history_tag;           //fw+
    // Reuse history.window to carry id; history_messages for reason      //fw+
    sf.variant.history.window = origId;                                  //fw+
    sf.variant.history.history_messages = reasonCode;                     //fw+

    meshtastic_MeshPacket *p = allocDataProtobuf(sf);
    if (!p) return;
    p->to = dest;
    p->decoded.portnum = meshtastic_PortNum_FWPLUS_CUSTODY_APP; //fw+
    p->want_ack = false;
    p->decoded.want_response = false;
    service->sendToMesh(p);
}
#include "modules/ModuleDev.h"
#include <Arduino.h>
#include <stdio.h> //fw+ snprintf
#include <iterator>
#include <map>
#include "fwplus_custody.h" //fw+

StoreForwardModule *storeForwardModule;

//fw+ Privacy policy: by default do NOT serve remote S&F client requests (HISTORY/STATS)
// to avoid leaking status/history to non-APK+ apps that render these frames as chat.
// This can be relaxed in future via Admin config when available.
static inline bool fwplus_allow_remote_sf_client_requests() { return false; }
//fw+ Do not serve history to PhoneAPI in FW+ variant (prevent chat spam in apps)
static inline bool fwplus_disable_phone_history() { return true; }

int32_t StoreForwardModule::runOnce()
{
#if defined(ARCH_ESP32) || defined(ARCH_PORTDUINO) || defined(ARCH_NRF52)
    //fw+ Allow server-only mode: run server loop when is_server, regardless of enabled flag
    if (is_server) {
        //fw+ process custody schedules before normal server loop
        processSchedules();
        // Send out the message queue.
        if (this->busy) {
            // Only send packets if the channel is less than 25% utilized and until historyReturnMax
            if (airTime->isTxAllowedChannelUtil(true) && this->requestCount < this->historyReturnMax) {
                if (!storeForwardModule->sendPayload(this->busyTo, this->last_time)) {
                    this->requestCount = 0;
                    this->busy = false;
                }
            }
        } else if (this->heartbeat && (!Throttle::isWithinTimespanMs(lastHeartbeat, heartbeatInterval * 1000)) &&
                   airTime->isTxAllowedChannelUtil(true)) {
            lastHeartbeat = millis();
            LOG_INFO("Send heartbeat");
            meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
            sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
            sf.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
            sf.variant.heartbeat.period = heartbeatInterval;
            sf.variant.heartbeat.secondary = 0; // TODO we always have one primary router for now
            storeForwardModule->sendMessage(NODENUM_BROADCAST, sf);
        }
#ifdef ARCH_PORTDUINO
        //fw+ Faster scheduling loop in simulation to ensure prompt S&F forwarding
        return 200;
#else
        return (this->packetTimeMax);
#endif
    }
#endif
    return disable();
}

/**
 * Populates the PSRAM with data to be sent later when a device is out of range.
 */
void StoreForwardModule::populatePSRAM()
{
    /*
    For PSRAM usage, see:
        https://learn.upesy.com/en/programmation/psram.html#psram-tab
    */

    LOG_DEBUG("Before PSRAM init: heap %d/%d PSRAM %d/%d", memGet.getFreeHeap(), memGet.getHeapSize(), memGet.getFreePsram(),
              memGet.getPsramSize());

    /* Use a maximum of 3/4 the available PSRAM unless otherwise specified.
        Note: This needs to be done after every thing that would use PSRAM
    */
    uint32_t numberOfPackets =
        (this->records ? this->records : (((memGet.getFreePsram() / 4) * 3) / sizeof(PacketHistoryStruct)));
    this->records = numberOfPackets;
#if defined(ARCH_ESP32)
    this->packetHistory = static_cast<PacketHistoryStruct *>(ps_calloc(numberOfPackets, sizeof(PacketHistoryStruct)));
#elif defined(ARCH_PORTDUINO)
    this->packetHistory = static_cast<PacketHistoryStruct *>(calloc(numberOfPackets, sizeof(PacketHistoryStruct)));

#endif

    LOG_DEBUG("After PSRAM init: heap %d/%d PSRAM %d/%d", memGet.getFreeHeap(), memGet.getHeapSize(), memGet.getFreePsram(),
              memGet.getPsramSize());
    LOG_DEBUG("numberOfPackets for packetHistory - %u", numberOfPackets);
}

/**
 * Sends messages from the message history to the specified recipient.
 *
 * @param sAgo The number of seconds ago from which to start sending messages.
 * @param to The recipient ID to send the messages to.
 */
void StoreForwardModule::historySend(uint32_t secAgo, uint32_t to)
{
    this->last_time = getTime() < secAgo ? 0 : getTime() - secAgo;
    uint32_t queueSize = getNumAvailablePackets(to, last_time);
    if (queueSize > this->historyReturnMax)
        queueSize = this->historyReturnMax;

    if (queueSize) {
        LOG_INFO("S&F - Send %u message(s)", queueSize);
        this->busy = true; // runOnce() will pickup the next steps once busy = true.
        this->busyTo = to;
    } else {
        LOG_INFO("S&F - No history");
    }
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
    sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
    sf.which_variant = meshtastic_StoreAndForward_history_tag;
    sf.variant.history.history_messages = queueSize;
    sf.variant.history.window = secAgo * 1000;
    sf.variant.history.last_request = lastRequest[to];
    //storeForwardModule->sendMessage(to, sf); //fw+ suppress S&F path (no HISTORY to phone)
    setIntervalFromNow(this->packetTimeMax); // Delay start of sending payloads
}

/**
 * Returns the number of available packets in the message history for a specified destination node.
 *
 * @param dest The destination node number.
 * @param last_time The relative time to start counting messages from.
 * @return The number of available packets in the message history.
 */
uint32_t StoreForwardModule::getNumAvailablePackets(NodeNum dest, uint32_t last_time)
{
    uint32_t count = 0;
    if (lastRequest.find(dest) == lastRequest.end()) {
        lastRequest.emplace(dest, 0);
    }
    for (uint32_t i = lastRequest[dest]; i < this->packetHistoryTotalCount; i++) {
        uint32_t t = this->packetHistory[i].time;
        if (t && (t > last_time)) {
            //fw+ skip delivered ids entirely
            if (isDelivered(this->packetHistory[i].id)) continue;
            //fw+ if another S&F claimed custody for this id, don't offer it to this client
            if (isClaimed(this->packetHistory[i].id)) continue;
            // Client is only interested in packets not from itself and only in broadcast packets or packets towards it.
            if (this->packetHistory[i].from != dest &&
                (this->packetHistory[i].to == NODENUM_BROADCAST || this->packetHistory[i].to == dest)) {
                count++;
            }
        }
    }
    return count;
}

/**
 * Allocates a mesh packet for sending to the phone.
 *
 * @return A pointer to the allocated mesh packet or nullptr if none is available.
 */
meshtastic_MeshPacket *StoreForwardModule::getForPhone()
{
    if (moduleConfig.store_forward.enabled && is_server) {
        NodeNum to = nodeDB->getNodeNum();
        //fw+ FW+: block serving history to phone entirely
        if (fwplus_disable_phone_history()) return nullptr;
        if (!this->busy) {
            // Get number of packets we're going to send in this loop
            uint32_t histSize = getNumAvailablePackets(to, 0); // No time limit
            if (histSize) {
                this->busy = true;
                this->busyTo = to;
            } else {
                return nullptr;
            }
        }

        // We're busy with sending to us until no payload is available anymore
        if (this->busy && this->busyTo == to) {
            meshtastic_MeshPacket *p = preparePayload(to, 0, true); // No time limit
            if (!p)                                                 // No more messages to send
                this->busy = false;
            return p;
        }
    }
    return nullptr;
}

/**
 * Adds a mesh packet to the history buffer for store-and-forward functionality.
 *
 * @param mp The mesh packet to add to the history buffer.
 */
void StoreForwardModule::historyAdd(const meshtastic_MeshPacket &mp)
{
    const auto &p = mp.decoded;

    if (this->packetHistoryTotalCount == this->records) {
        LOG_WARN("S&F - PSRAM Full. Starting overwrite");
        this->packetHistoryTotalCount = 0;
        for (auto &i : lastRequest) {
            i.second = 0; // Clear the last request index for each client device
        }
    }

    this->packetHistory[this->packetHistoryTotalCount].time = getTime();
    this->packetHistory[this->packetHistoryTotalCount].to = mp.to;
    this->packetHistory[this->packetHistoryTotalCount].channel = mp.channel;
    this->packetHistory[this->packetHistoryTotalCount].from = getFrom(&mp);
    this->packetHistory[this->packetHistoryTotalCount].id = mp.id;
    this->packetHistory[this->packetHistoryTotalCount].reply_id = p.reply_id;
    this->packetHistory[this->packetHistoryTotalCount].emoji = (bool)p.emoji;
    this->packetHistory[this->packetHistoryTotalCount].want_ack = mp.want_ack; //fw+
    this->packetHistory[this->packetHistoryTotalCount].encrypted = false;
    this->packetHistory[this->packetHistoryTotalCount].payload_size = p.payload.size;
    this->packetHistory[this->packetHistoryTotalCount].rx_rssi = mp.rx_rssi;
    this->packetHistory[this->packetHistoryTotalCount].rx_snr = mp.rx_snr;
    memcpy(this->packetHistory[this->packetHistoryTotalCount].payload, p.payload.bytes, meshtastic_Constants_DATA_PAYLOAD_LEN);

    this->packetHistoryTotalCount++;
    //fw+ if this id was previously marked delivered (race), neutralize immediately
    if (isDelivered(mp.id)) {
        this->packetHistory[this->packetHistoryTotalCount - 1].time = 0;
        LOG_DEBUG("fw+ historyAdd: neutralized delivered id=0x%x on insert", mp.id);
    }

    //fw+ if schedule was waiting for this id, fill it now
    auto it = pendingSchedule.find(mp.id);
    if (it != pendingSchedule.end()) {
        scheduleFromHistory(mp.id);
        pendingSchedule.erase(it);
    }
    //fw+ proactive: schedule on hear (no CR/CA needed)
    if (is_server) {
        //fw+ deferred custody: do not emit CA here; only schedule consideration
        scheduleFromHistory(mp.id);
        if (scheduleById.find(mp.id) == scheduleById.end()) {
            pendingSchedule[mp.id] = true;
        }
    }
}

//fw+ Opaque custody: store encrypted payload bytes when we cannot decode
void StoreForwardModule::historyAddOpaque(const meshtastic_MeshPacket &mp)
{
    if (this->packetHistoryTotalCount == this->records) {
        LOG_WARN("S&F - DRAM Full (opaque). Starting overwrite");
        this->packetHistoryTotalCount = 0;
        for (auto &i : lastRequest) { i.second = 0; }
    }

    this->packetHistory[this->packetHistoryTotalCount].time = getTime();
    this->packetHistory[this->packetHistoryTotalCount].to = mp.to;
    this->packetHistory[this->packetHistoryTotalCount].channel = mp.channel;
    this->packetHistory[this->packetHistoryTotalCount].from = getFrom(&mp);
    this->packetHistory[this->packetHistoryTotalCount].id = mp.id;
    this->packetHistory[this->packetHistoryTotalCount].reply_id = 0;
    this->packetHistory[this->packetHistoryTotalCount].emoji = false;
    this->packetHistory[this->packetHistoryTotalCount].want_ack = mp.want_ack; //fw+
    this->packetHistory[this->packetHistoryTotalCount].encrypted = true;
    // copy encrypted payload
    size_t copyLen = mp.encrypted.size;
    if (copyLen > sizeof(this->packetHistory[this->packetHistoryTotalCount].payload))
        copyLen = sizeof(this->packetHistory[this->packetHistoryTotalCount].payload);
    memcpy(this->packetHistory[this->packetHistoryTotalCount].payload, mp.encrypted.bytes, copyLen);
    this->packetHistory[this->packetHistoryTotalCount].payload_size = (pb_size_t)copyLen;
    this->packetHistory[this->packetHistoryTotalCount].rx_rssi = mp.rx_rssi;
    this->packetHistory[this->packetHistoryTotalCount].rx_snr = mp.rx_snr;

    this->packetHistoryTotalCount++;

    if (isDelivered(mp.id)) {
        this->packetHistory[this->packetHistoryTotalCount - 1].time = 0;
        LOG_DEBUG("fw+ historyAddOpaque: neutralized delivered id=0x%x on insert", mp.id);
    }

    LOG_DEBUG("fw+ Opaque store id=0x%x bytes=%u", mp.id, (unsigned)this->packetHistory[this->packetHistoryTotalCount - 1].payload_size);

    //fw+ Schedule delivery similar to decoded path
    auto it = pendingSchedule.find(mp.id);
    if (it != pendingSchedule.end()) {
        scheduleFromHistory(mp.id);
        pendingSchedule.erase(it);
    }
    if (is_server) {
        //fw+ deferred custody for opaque: no immediate CA; just schedule consideration
        scheduleFromHistory(mp.id);
        if (scheduleById.find(mp.id) == scheduleById.end()) {
            pendingSchedule[mp.id] = true;
        }
    }
}

/**
 * Sends a payload to a specified destination node using the store and forward mechanism.
 *
 * @param dest The destination node number.
 * @param last_time The relative time to start sending messages from.
 * @return True if a packet was successfully sent, false otherwise.
 */
bool StoreForwardModule::sendPayload(NodeNum dest, uint32_t last_time)
{
    meshtastic_MeshPacket *p = preparePayload(dest, last_time);
    if (p) {
        LOG_INFO("Send S&F Payload");
        service->sendToMesh(p);
        this->requestCount++;
        return true;
    }
    return false;
}

/**
 * Prepares a payload to be sent to a specified destination node from the S&F packet history.
 *
 * @param dest The destination node number.
 * @param last_time The relative time to start sending messages from.
 * @return A pointer to the prepared mesh packet or nullptr if none is available.
 */
meshtastic_MeshPacket *StoreForwardModule::preparePayload(NodeNum dest, uint32_t last_time, bool local)
{
    for (uint32_t i = lastRequest[dest]; i < this->packetHistoryTotalCount; i++) {
        if (this->packetHistory[i].time && (this->packetHistory[i].time > last_time)) {
            //fw+ skip delivered ids entirely
                if (isDelivered(this->packetHistory[i].id)) continue;
                //fw+ skip globally failed ids to avoid recreating schedules on HISTORY
                if (isFailed(this->packetHistory[i].id)) continue;
            //fw+ skip if custody claimed elsewhere
            if (isClaimed(this->packetHistory[i].id)) continue;
            /*  Copy the messages that were received by the server in the last msAgo
                to the packetHistoryTXQueue structure.
                Client not interested in packets from itself and only in broadcast packets or packets towards it. */
            if (this->packetHistory[i].from != dest &&
                (this->packetHistory[i].to == NODENUM_BROADCAST || this->packetHistory[i].to == dest)) {

                meshtastic_MeshPacket *p = allocDataPacket();

                p->to = local ? this->packetHistory[i].to : dest; // PhoneAPI can handle original `to`
                p->from = this->packetHistory[i].from;
                p->id = this->packetHistory[i].id;
                p->channel = this->packetHistory[i].channel;
                p->decoded.reply_id = this->packetHistory[i].reply_id;
                p->rx_time = this->packetHistory[i].time;
                p->decoded.emoji = (uint32_t)this->packetHistory[i].emoji;
                p->rx_rssi = this->packetHistory[i].rx_rssi;
                p->rx_snr = this->packetHistory[i].rx_snr;

                // Let's assume that if the server received the S&F request that the client is in range.
                //   TODO: Make this configurable.
                //fw+ For server-side delivery: request ACK for DM to allow source to see success
                bool isDM = (this->packetHistory[i].to != NODENUM_BROADCAST &&
                             this->packetHistory[i].to != NODENUM_BROADCAST_NO_LORA);
                p->want_ack = isDM;
                p->decoded.want_response = false;

                if (local) { // PhoneAPI gets normal TEXT_MESSAGE_APP
                    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
                    memcpy(p->decoded.payload.bytes, this->packetHistory[i].payload, this->packetHistory[i].payload_size);
                    p->decoded.payload.size = this->packetHistory[i].payload_size;
                } else {
                    if (this->packetHistory[i].encrypted) {
                        //fw+ Opaque forward: re-send encrypted bytes unchanged; hide decoded metadata
                        p->decoded.portnum = (meshtastic_PortNum)0;
                        p->decoded.payload.size = 0;
                        p->decoded.reply_id = 0;
                        p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
                        memcpy(p->encrypted.bytes, this->packetHistory[i].payload, this->packetHistory[i].payload_size);
                        p->encrypted.size = this->packetHistory[i].payload_size;
                    } else {
                        meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
                        sf.which_variant = meshtastic_StoreAndForward_text_tag;
                        sf.variant.text.size = this->packetHistory[i].payload_size;
                        memcpy(sf.variant.text.bytes, this->packetHistory[i].payload, this->packetHistory[i].payload_size);
                        if (this->packetHistory[i].to == NODENUM_BROADCAST) {
                            sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST;
                        } else {
                            sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT;
                        }
                        p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes),
                                                                     &meshtastic_StoreAndForward_msg, &sf);
                    }
                }

                lastRequest[dest] = i + 1; // Update the last request index for the client device

                return p;
            }
        }
    }
    return nullptr;
}

/**
 * Sends a message to a specified destination node using the store and forward protocol.
 *
 * @param dest The destination node number.
 * @param payload The message payload to be sent.
 */
void StoreForwardModule::sendMessage(NodeNum dest, const meshtastic_StoreAndForward &payload)
{
    meshtastic_MeshPacket *p = allocDataProtobuf(payload);

    p->to = dest;

    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    // Let's assume that if the server received the S&F request that the client is in range.
    //   TODO: Make this configurable.
    p->want_ack = false;
    p->decoded.want_response = false;

    service->sendToMesh(p);
}

/**
 * Sends a store-and-forward message to the specified destination node.
 *
 * @param dest The destination node number.
 * @param rr The store-and-forward request/response message to send.
 */
void StoreForwardModule::sendMessage(NodeNum dest, meshtastic_StoreAndForward_RequestResponse rr)
{
    // Craft an empty response, save some bytes in flash
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
    sf.rr = rr;
    storeForwardModule->sendMessage(dest, sf);
}

/**
 * Sends a text message with an error (busy or channel not available) to the specified destination node.
 *
 * @param dest The destination node number.
 * @param want_response True if the original message requested a response, false otherwise.
 */
void StoreForwardModule::sendErrorTextMessage(NodeNum dest, bool want_response)
{
    meshtastic_MeshPacket *pr = allocDataPacket();
    pr->to = dest;
    pr->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    pr->want_ack = false;
    pr->decoded.want_response = false;
    pr->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    const char *str;
    if (this->busy) {
        str = "S&F - Busy. Try again shortly.";
    } else {
        str = "S&F not permitted on the public channel.";
    }
    LOG_WARN("%s", str);
    memcpy(pr->decoded.payload.bytes, str, strlen(str));
    pr->decoded.payload.size = strlen(str);
    if (want_response) {
        ignoreRequest = true; // This text message counts as response.
    }
    service->sendToMesh(pr);
}

/**
 * Sends statistics about the store and forward module to the specified node.
 *
 * @param to The node ID to send the statistics to.
 */
void StoreForwardModule::statsSend(uint32_t to)
{
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;

    sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
    sf.which_variant = meshtastic_StoreAndForward_stats_tag;
    sf.variant.stats.messages_total = this->records;
    sf.variant.stats.messages_saved = this->packetHistoryTotalCount;
    sf.variant.stats.messages_max = this->records;
    sf.variant.stats.up_time = millis() / 1000;
    sf.variant.stats.requests = this->requests;
    sf.variant.stats.requests_history = this->requests_history;
    sf.variant.stats.heartbeat = this->heartbeat;
    sf.variant.stats.return_max = this->historyReturnMax;
    sf.variant.stats.return_window = this->historyReturnWindow;

    LOG_DEBUG("Send S&F Stats");
    storeForwardModule->sendMessage(to, sf);
}

/**
 * Handles a received mesh packet, potentially storing it for later forwarding.
 *
 * @param mp The received mesh packet.
 * @return A `ProcessMessage` indicating whether the packet was successfully handled.
 */
ProcessMessage StoreForwardModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(ARCH_ESP32) || defined(ARCH_PORTDUINO) || defined(ARCH_NRF52)
    //fw+ Allow server-only mode: process server paths even if module disabled
    if (moduleConfig.store_forward.enabled || is_server) {
        //fw+ If we hear the destination as a sender, clear its cooldown (node is reachable again)
        if (is_server) {
            NodeNum seen = getFrom(&mp);
            if (seen && destCooldownUntilMs.find(seen) != destCooldownUntilMs.end()) {
                clearDestCooldown(seen);
            }
        }

        if ((mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) && is_server) {
            auto &p = mp.decoded;
            if (isToUs(&mp) && (p.payload.bytes[0] == 'S') && (p.payload.bytes[1] == 'F') && (p.payload.bytes[2] == 0x00)) {
                LOG_DEBUG("Legacy Request to send");

                //fw+ FW+: block legacy 'SF\0' history requests from any client
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
            } else {
                storeForwardModule->historyAdd(mp);
                LOG_INFO("S&F stored. Active history entries: %u", (unsigned)countActiveHistory());
            }
        } else if (is_server && mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag && !isBroadcast(mp.to)) {
            //fw+ Opaque custody path for encrypted DM we can't decode locally
            LOG_DEBUG("fw+ Encrypted DM captured id=0x%x", mp.id);
            storeForwardModule->historyAddOpaque(mp);
            // For DM, decide if CA should be deferred (near/healthy) or immediate (far/uncertain)
            if (mp.to != NODENUM_BROADCAST && mp.to != NODENUM_BROADCAST_NO_LORA) {
                NodeNum src = getFrom(&mp);
                NodeNum dst = mp.to;
                if (shouldDeferCustodyAckForEncrypted(src, dst)) {
                    // Defer CA: schedule CA to be sent just before first server attempt
                    // We piggyback on the normal schedule; CA is emitted JIT in processSchedules when tries==0
                    // No immediate CA here
                } else {
                    // Immediate CA: stop source retransmissions quickly
                    sendCustodyAck(src, mp.id);
                    markClaimed(mp.id);
                }
            }
        } else if (!isFromUs(&mp) && mp.decoded.portnum == meshtastic_PortNum_STORE_FORWARD_APP) {
            auto &p = mp.decoded;
            meshtastic_StoreAndForward scratch;
            meshtastic_StoreAndForward *decoded = NULL;
            if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
                if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_StoreAndForward_msg, &scratch)) {
                    decoded = &scratch;
                } else {
                    LOG_ERROR("Error decoding proto module!");
                    // if we can't decode it, nobody can process it!
                    return ProcessMessage::STOP;
                }
                //fw+ FW+: Never pass StoreForward frames beyond this module; always stop propagation to PhoneAPI
                (void)handleReceivedProtobuf(mp, decoded);
                return ProcessMessage::STOP;
            }
        } // all others are irrelevant
    }
    else {
        //fw+ FW+: Even when S&F client/server is disabled, consume S&F frames to suppress PhoneAPI spam
        if (!isFromUs(&mp) && mp.decoded.portnum == meshtastic_PortNum_STORE_FORWARD_APP &&
            mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
            meshtastic_StoreAndForward scratch;
            if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size,
                                     &meshtastic_StoreAndForward_msg, &scratch)) {
                // Reuse suppression logic in protobuf handler; if it returns handled, stop propagation
                if (handleReceivedProtobuf(mp, &scratch)) {
                    return ProcessMessage::STOP;
                }
            }
        }
    }

#endif

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

/**
 * Handles a received protobuf message for the Store and Forward module.
 *
 * @param mp The received MeshPacket to handle.
 * @param p A pointer to the StoreAndForward object.
 * @return True if the message was successfully handled, false otherwise.
 */
bool StoreForwardModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_StoreAndForward *p)
{
    //fw+ Allow server-only mode to handle protobuf even if module disabled
    if (!moduleConfig.store_forward.enabled && !is_server) {
        // If neither module is enabled nor server-only, don't handle
        return false;
    }

    requests++;

    switch (p->rr) {
    case meshtastic_StoreAndForward_RequestResponse_CLIENT_ERROR:
    case meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT:
        if (is_server) {
            // stop sending stuff, the client wants to abort or has another error
            if ((this->busy) && (this->busyTo == getFrom(&mp))) {
                LOG_ERROR("Client in ERROR or ABORT requested");
                this->requestCount = 0;
                this->busy = false;
            }
        }
        //fw+ Suppress forwarding STATS to phone regardless of client/server flags
        return true;

    case meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY:
        if (is_server) {
            //fw+ Privacy: block serving HISTORY to remote clients unless explicitly allowed
            if (!fwplus_allow_remote_sf_client_requests()) {
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
                break;
            }
            requests_history++;
            LOG_INFO("Client Request to send HISTORY");
            // fw+ In mini-server mode, do not serve history replays to conserve RAM
            if (miniServerMode) {
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
                break;
            }
            //fw+ FW+: never serve history replays (prevent chat spam in clients)
            if (fwplus_disable_phone_history()) {
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
                break;
            }
            // Send the last 60 minutes of messages.
            if (this->busy || channels.isDefaultChannel(mp.channel)) {
                sendErrorTextMessage(getFrom(&mp), mp.decoded.want_response);
            } else {
                if ((p->which_variant == meshtastic_StoreAndForward_history_tag) && (p->variant.history.window > 0)) {
                    // window is in minutes
                    storeForwardModule->historySend(p->variant.history.window * 60, getFrom(&mp));
                } else {
                    storeForwardModule->historySend(historyReturnWindow * 60, getFrom(&mp)); // defaults to 4 hours
                }
            }
        }
        //fw+ Suppress forwarding HISTORY header to phone regardless of client/server flags
        return true;

    case meshtastic_StoreAndForward_RequestResponse_CLIENT_PING:
        if (is_server) {
            // respond with a ROUTER PONG
            storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG);
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_CLIENT_PONG:
        if (is_server) {
            // NodeDB is already updated
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS:
        if (is_server) {
            //fw+ Privacy: block serving STATS to remote clients unless explicitly allowed
            if (!fwplus_allow_remote_sf_client_requests()) {
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
                LOG_INFO("S&F - Remote STATS blocked by privacy policy");
                break;
            }
            LOG_INFO("Client Request to send STATS");
            if (this->busy) {
                storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
                LOG_INFO("S&F - Busy. Try again shortly");
            } else {
                storeForwardModule->statsSend(getFrom(&mp));
            }
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_ERROR:
    case meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY:
        if (is_client) {
            LOG_DEBUG("StoreAndForward_RequestResponse_ROUTER_BUSY");
            // retry in messages_saved * packetTimeMax ms
            retry_delay = millis() + getNumAvailablePackets(this->busyTo, this->last_time) * packetTimeMax *
                                         (meshtastic_StoreAndForward_RequestResponse_ROUTER_ERROR ? 2 : 1);
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG:
    // A router responded, this is equal to receiving a heartbeat
    case meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT:
        if (is_client) {
            // register heartbeat and interval
            if (p->which_variant == meshtastic_StoreAndForward_heartbeat_tag) {
                heartbeatInterval = p->variant.heartbeat.period;
            }
            lastHeartbeat = millis();
            LOG_INFO("StoreAndForward Heartbeat received");
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_PING:
        if (is_client) {
            // respond with a CLIENT PONG
            storeForwardModule->sendMessage(getFrom(&mp), meshtastic_StoreAndForward_RequestResponse_CLIENT_PONG);
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS:
        if (is_client) {
            LOG_DEBUG("Router Response STATS");
            // These fields only have informational purpose on a client. Fill them to consume later.
            if (p->which_variant == meshtastic_StoreAndForward_stats_tag) {
                this->records = p->variant.stats.messages_max;
                this->requests = p->variant.stats.requests;
                this->requests_history = p->variant.stats.requests_history;
                this->heartbeat = p->variant.stats.heartbeat;
                this->historyReturnMax = p->variant.stats.return_max;
                this->historyReturnWindow = p->variant.stats.return_window;
            }
            //fw+ Do not forward STATS to phone (FW+ privacy/UI hygiene)
            return true;
        }
        break;

    case meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY:
        if (is_client) {
            // These fields only have informational purpose on a client. Fill them to consume later.
            if (p->which_variant == meshtastic_StoreAndForward_history_tag) {
                this->historyReturnWindow = p->variant.history.window / 60000;
                LOG_INFO("Router Response HISTORY - Sending %d messages from last %d minutes",
                         p->variant.history.history_messages, this->historyReturnWindow);
            }
            //fw+ Do not forward HISTORY headers to phone (FW+ privacy/UI hygiene)
            return true;
        }
        break;

    default: {
        //fw+ Suppress S&F TEXT frames from reaching PhoneAPI (avoid chat spam)
        if (p->which_variant == meshtastic_StoreAndForward_text_tag &&
            (p->rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT ||
             p->rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST)) {
            return true;
        }
        //fw+ DTN-like: interpret custom custody RR codes using control variants (no text payload)
        if (p->rr == fwplus_custody::RR_ROUTER_CUSTODY_ACK || p->rr == fwplus_custody::RR_ROUTER_DELIVERED ||
            p->rr == fwplus_custody::RR_ROUTER_CUSTODY_CLAIM) {
            //fw+ passive discovery: note FW+ S&F presence
            markSfServerSeen(getFrom(&mp));
            uint32_t id = 0;
            if (p->which_variant == meshtastic_StoreAndForward_history_tag) {
                id = p->variant.history.window; // CA/DR carry id here
            }

            if (id) {
                if (is_client) {
                    if (p->rr == fwplus_custody::RR_ROUTER_CUSTODY_ACK) {
                        LOG_INFO("fw+ Custody ACK for id=0x%x from router", id);
                    } else if (p->rr == fwplus_custody::RR_ROUTER_CUSTODY_CLAIM) {
                        LOG_INFO("fw+ Custody CLAIM for id=0x%x from router", id);
                        //fw+ honor claim: avoid taking custody for this id
                        markClaimed(id);
                    } else {
                        LOG_INFO("fw+ Delivered notice for id=0x%x from router", id);
                        //fw+ Adapt DV-ETX: reward path towards source using last relay hint if available
                        if (router) {
                            uint8_t via = (mp.relay_node != NO_RELAY_NODE) ? mp.relay_node : NO_NEXT_HOP_PREFERENCE;
                            router->rewardRouteOnDelivered(mp.id, getFrom(&mp), via, mp.rx_snr);
                        }
                    }
                }
            }
            //fw+ Do not forward control (CA/CR/DR) to phone
            return true;
        }
        //fw+ DELIVERY_FAILED mapping with reason in history.history_messages
        if (p->rr == fwplus_custody::RR_ROUTER_DELIVERY_FAILED) {
            //fw+ passive discovery: note FW+ S&F presence
            markSfServerSeen(getFrom(&mp));
            uint32_t id = 0;
            uint32_t reason = 0;
            if (p->which_variant == meshtastic_StoreAndForward_history_tag) {
                id = p->variant.history.window;
                reason = p->variant.history.history_messages;
            }
            if (id && is_client) {
                LOG_WARN("fw+ Delivery FAILED for id=0x%x reason=%u", id, (unsigned)reason);
                if (router) {
                    uint8_t via = (mp.relay_node != NO_RELAY_NODE) ? mp.relay_node : NO_NEXT_HOP_PREFERENCE;
                    router->penalizeRouteOnFailed(mp.id, getFrom(&mp), via, reason);
                }
                //fw+ Global suppression of further replays for this id on this node
                markFailed(id);
                clearHistoryById(id);
            }
            //fw+ Do not forward DF control to phone
            return true;
        }
        break; // no need to do anything more
    }
    }
    return false; // RoutingModule sends it to the phone
}

//fw+ helpers
void StoreForwardModule::sendCustodyAck(NodeNum to, uint32_t origId)
{
    //fw+ gate on single control: prefer NodeModAdmin when available; fallback to StoreForward flag
    bool allow = false;
    allow = moduleConfig.nodemodadmin.emit_custody_control_signals;
    if (!allow) return;

    //fw+ use private custody control port to avoid apps rendering S&F HISTORY frames
    sendCustodyControlPrivate(to, (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_CUSTODY_ACK, origId, 0);
    //fw+ stats
    custodyCountCA++; lastCAms = nowMs();
}

//fw+ Broadcast a custody-claim (CR) so other S&F servers back off for this id
void StoreForwardModule::sendCustodyClaim(uint32_t origId)
{
    bool allow = false;
    allow = moduleConfig.nodemodadmin.emit_custody_control_signals;
    if (!allow) return;

    //fw+ Gate claim broadcast on long peer detection (12h) and channel utilization
    bool peersSeen = hasRecentSfPeers(12 * 60 * 60 * 1000UL);
    bool chanOk = (!airTime) || (airTime->max_channel_util_percent < 40);
    if (!peersSeen || !chanOk) {
        LOG_DEBUG("fw+ Skip CR broadcast (peersSeen=%d chanOk=%d)", (int)peersSeen, (int)chanOk);
        return;
    }

    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
    sf.rr = (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_CUSTODY_CLAIM; //fw+
    sf.which_variant = meshtastic_StoreAndForward_history_tag;
    sf.variant.history.window = origId; // carry id
    //storeForwardModule->sendMessage(NODENUM_BROADCAST, sf); //fw+ suppress S&F path
    sendCustodyControlPrivate(NODENUM_BROADCAST, (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_CUSTODY_CLAIM, origId, 0);
    custodyCountCR++; lastCRms = nowMs();
}

//fw+ Broadcast a custody-delivered (DR) so other servers can drop it from history if desired
void StoreForwardModule::sendCustodyDelivered(uint32_t origId)
{
    bool allow = false;
    allow = moduleConfig.nodemodadmin.emit_custody_control_signals;
    if (!allow) return;

    //fw+ Gate DR broadcast on long peer detection (12h) and channel utilization
    bool peersSeen = hasRecentSfPeers(12 * 60 * 60 * 1000UL);
    bool chanOk = (!airTime) || (airTime->max_channel_util_percent < 40);
    if (!peersSeen || !chanOk) {
        LOG_DEBUG("fw+ Skip DR broadcast (peersSeen=%d chanOk=%d)", (int)peersSeen, (int)chanOk);
        return;
    }

    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
    sf.rr = (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_DELIVERED; //fw+
    sf.which_variant = meshtastic_StoreAndForward_history_tag;
    sf.variant.history.window = origId;
    //storeForwardModule->sendMessage(NODENUM_BROADCAST, sf); //fw+ suppress S&F path
    sendCustodyControlPrivate(NODENUM_BROADCAST, (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_DELIVERED, origId, 0);
    custodyCountDR++; lastDRms = nowMs();
}

//fw+ Broadcast a delivery-failed (DF) so other servers can drop and sources can mark failure
static inline bool isTerminalNak(meshtastic_Routing_Error e)
{
    return (e == meshtastic_Routing_Error_NO_CHANNEL || e == meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY);
}

void StoreForwardModule::sendDeliveryFailed(uint32_t origId, uint32_t reasonCode)
{
    bool allow = false;
    allow = moduleConfig.nodemodadmin.emit_custody_control_signals;
    if (!allow) return;

    //fw+ Gate DF broadcast on long peer detection (12h) and channel utilization
    bool peersSeen = hasRecentSfPeers(12 * 60 * 60 * 1000UL);
    bool chanOk = (!airTime) || (airTime->max_channel_util_percent < 40);
    if (!peersSeen || !chanOk) {
        LOG_DEBUG("fw+ Skip DF broadcast (peersSeen=%d chanOk=%d)", (int)peersSeen, (int)chanOk);
        return;
    }

    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_zero;
    sf.rr = (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_DELIVERY_FAILED; //fw+
    sf.which_variant = meshtastic_StoreAndForward_history_tag;
    // Reuse window for id; encode reason into history_messages for compactness
    sf.variant.history.window = origId;
    sf.variant.history.history_messages = reasonCode;
    //storeForwardModule->sendMessage(NODENUM_BROADCAST, sf); //fw+ suppress S&F path
    sendCustodyControlPrivate(NODENUM_BROADCAST, (meshtastic_StoreAndForward_RequestResponse)fwplus_custody::RR_ROUTER_DELIVERY_FAILED, origId, reasonCode);
    custodyCountDF++; lastDFms = nowMs();
}

//fw+ cancel schedule on destination ACK and mark delivered
void StoreForwardModule::cancelScheduleOnAck(uint32_t id, NodeNum ackFrom)
{
    auto it = scheduleById.find(id);
    if (it == scheduleById.end()) return;
    const CustodySchedule &s = it->second;
    if (s.isDM && s.to == ackFrom) {
        scheduleById.erase(it);
        markDelivered(id);
        //fw+ broadcast delivered control for FW+ sources; ignored by stock nodes/APK
        sendCustodyDelivered(id);
    }
}

//fw+ Mark id delivered and neutralize history entries so they won't be counted/sent again
void StoreForwardModule::markDelivered(uint32_t id)
{
    deliveredIds.insert(id);
    uint32_t cleared = clearHistoryById(id);
    LOG_INFO("fw+ S&F delivered id=0x%x, cleared %u history entries", id, (unsigned)cleared);
}

uint32_t StoreForwardModule::clearHistoryById(uint32_t id)
{
    uint32_t cleared = 0;
    for (uint32_t i = 0; i < this->packetHistoryTotalCount; i++) {
        if (this->packetHistory[i].id == id) {
            // zero time marks as invalid for scans without touching counters structure
            this->packetHistory[i].time = 0;
            cleared++;
        }
    }
    return cleared;
}

uint32_t StoreForwardModule::countActiveHistory() const
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < this->packetHistoryTotalCount; i++) {
        if (this->packetHistory[i].time) c++;
    }
    return c;
}

uint32_t StoreForwardModule::addJitter(uint32_t ms, uint8_t pct) const
{
    if (pct == 0) return ms;
    uint32_t span = (ms * pct) / 100;
    uint32_t r = random(2 * span + 1); // 0..2span??
    int32_t offset = (int32_t)r - (int32_t)span;
    int64_t out = (int64_t)ms + offset;
    if (out < 0) out = 0;
    return (uint32_t)out;
}

uint8_t StoreForwardModule::estimateHops(NodeNum to) const
{
    meshtastic_NodeInfoLite *n = nodeDB ? nodeDB->getMeshNode(to) : nullptr;
    if (n && n->has_hops_away && n->hops_away > 0 && n->hops_away <= 7) return (uint8_t)n->hops_away;
    return 8; // fallback conservative
}

uint32_t StoreForwardModule::computeInitialDelayMs(uint8_t estHops) const
{
    // Target: 10–30s normal; 15–45s in dense mesh mode
    uint32_t cap = 30000;
    uint32_t base0 = 10000;
    //fw+ apply dense overrides from NodeModAdmin (optional) or auto-dense heuristic
    bool dense = isDenseEnvironment();
    if (moduleConfig.nodemodadmin.dense_initial_cap_secs) {
        dense = true;
        uint32_t v = moduleConfig.nodemodadmin.dense_initial_cap_secs * 1000;
        if (v > cap) cap = v;
        base0 = 15000;
    }
    if (dense) {
        cap = (cap < 45000 ? 45000 : cap); // default to 45s cap if none provided
        base0 = 15000;
    }
    uint32_t base = base0 + (uint32_t)(dmHopCoefMs * 2.0f) * estHops;
    if (base > cap) base = cap;
    return addJitter(base, dmJitterPct);
}

uint32_t StoreForwardModule::computeRetryDelayMs(uint8_t tries, uint8_t estHops, uint32_t lastTxMs, uint32_t now) const
{
    // Retry pacing: minimum 2 minutes per hop (server-relative), with jitter
    uint32_t minBase = 60000;
    //fw+ apply dense retry spacing from NodeModAdmin (optional) or auto-dense heuristic
    bool dense = isDenseEnvironment();
    if (moduleConfig.nodemodadmin.dense_min_retry_secs) {
        dense = true;
        uint32_t v = moduleConfig.nodemodadmin.dense_min_retry_secs * 1000;
        if (v > minBase) minBase = v;
    }
    if (dense && minBase < 90000) minBase = 90000; // default 90s if none provided
    uint32_t hops = estHops ? estHops : 1;
    uint32_t hopSpacing = 120000UL * hops; // 2 min per hop
    uint32_t base = (hopSpacing > minBase) ? hopSpacing : minBase;
    uint32_t target = now + addJitter(base, dmJitterPct);
    if (lastTxMs && target < lastTxMs + minRetrySpacingMs) target = lastTxMs + minRetrySpacingMs;
    return target;
}

//fw+ Heuristic to detect dense/contended environments based on NodeDB and channel utilization
bool StoreForwardModule::isDenseEnvironment() const
{
    //fw+ Dense if we see many local-online nodes (non-MQTT) or global nodes, or high polite/max channel utilization
    uint32_t total = 0;
    if (nodeDB) {
        total = nodeDB->getNumMeshNodes();
    }
    //fw+ Treat as dense when total visible nodes are high
    bool manyNodes = (total >= 100);
    bool highUtil = false;
    if (airTime) {
        //fw+ Consider dense when polite utilization is high; thresholds are conservative
        highUtil = (airTime->polite_channel_util_percent >= 25) || (airTime->max_channel_util_percent >= 35);
    }
    return manyNodes || highUtil;
}

void StoreForwardModule::scheduleFromHistory(uint32_t id)
{
    // Find last matching packet in history (simple linear scan from tail)
    for (int32_t i = (int32_t)this->packetHistoryTotalCount - 1; i >= 0; --i) {
        if (this->packetHistory[i].id == id) {
            //fw+ do not schedule delivered entries
            if (isDelivered(id) || isFailed(id) || isClaimed(id)) return;
            //fw+ Gating: require DM (unicast) with original want_ack, freshness, DV-ETX
            NodeNum dest = this->packetHistory[i].to;
            bool isDM = (dest != NODENUM_BROADCAST && dest != NODENUM_BROADCAST_NO_LORA);
            if (!isDM) return; // do not take custody for broadcasts
            if (!this->packetHistory[i].want_ack) return; // only take custody for packets that wanted ACK
            //fw+ Per-destination cooldown after DF
            if (isDestCooled(dest)) {
                LOG_DEBUG("fw+ Skip schedule id=0x%x due to dest cooldown %u ms left", id, (unsigned)(destCooldownUntilMs[dest] - nowMs()));
                return;
            }
            //fw+ Fast path: if destination appears near and route is healthy, prefer native delivery and do not schedule
            uint8_t estHopsNear = estimateHops(dest);
            if (estHopsNear <= 1 && isDestFresh(dest) && hasSufficientRouteConfidence(dest)) {
                LOG_DEBUG("fw+ Near/healthy path detected, skip S&F schedule id=0x%x dest=%u", id, (unsigned)dest);
                return;
            }
            if (estimateHops(dest) > forwardMaxHops || !isDestFresh(dest) || !hasSufficientRouteConfidence(dest)) {
                // Do not attempt; globally announce DF to stop others and mark failed locally
                broadcastDeliveryFailedControl(id, (uint32_t)meshtastic_Routing_Error_NO_ROUTE);
                markFailed(id);
                //fw+ start per-destination cooldown as route is likely not viable now
                startDestCooldown(dest);
                clearHistoryById(id);
                return;
            }
            CustodySchedule s{};
            s.id = id;
            s.to = this->packetHistory[i].to;
            s.isDM = true;
            s.tries = 0;
            s.createdMs = nowMs();
            //fw+ scale initial delay by estimated hops (dense mesh settling)
            uint8_t estHops = estimateHops(s.to);
            uint32_t base = s.isDM ? computeInitialDelayMs(estHops) : (random(bcMaxDelayMs - bcMinDelayMs + 1) + bcMinDelayMs);
            //fw+ if we saw other FW+ S&F recently (12h), increase initial delay slightly to reduce races
            if (hasRecentSfPeers(12 * 60 * 60 * 1000UL)) { // last 12 hours
                uint32_t extra = (estHops + 1) * 500; // 0.5s per hop, min 0.5s
                base += extra;
            }
#ifdef ARCH_PORTDUINO
            //fw+ In simulation, schedule DM sooner to observe forwarding quickly
            if (s.isDM) base = 400; // ~0.4s
#endif
            //fw+ adaptive grace: compute initialGrace and prefer native delivery during grace
            s.initialGraceMs = computeAdaptiveGraceMs(s.to, this->packetHistory[i]);
            //fw+ never schedule in the past
            s.nextAttemptMs = nowMs() + (s.isDM ? base : addJitter(base, bcJitterPct));
            //fw+ set overall deadline (5 min from custody start)
            s.deadlineMs = nowMs() + dmMaxDelayMs;
            scheduleById[id] = s;
            //fw+ do NOT announce claim yet; send CA/CR just-in-time before first transmit
            LOG_INFO("fw+ S&F schedule id=0x%x in %u ms (DM=%d)", id, (unsigned)(s.nextAttemptMs - nowMs()), (int)s.isDM);
            break;
        }
    }
}

void StoreForwardModule::scheduleCustodyIfReady(uint32_t id)
{
    // If we already scheduled, skip
    if (scheduleById.find(id) != scheduleById.end()) return;
    scheduleFromHistory(id);
}

void StoreForwardModule::processSchedules()
{
    uint32_t now = nowMs();
    //fw+ Global DM concurrency cap: track DM sends issued in this pass
    uint32_t dmSendsThisPass = 0;
    for (auto it = scheduleById.begin(); it != scheduleById.end();) {
        CustodySchedule &s = it->second;
        //fw+ remove delivered schedules early
        if (isDelivered(s.id)) { it = scheduleById.erase(it); continue; }
        //fw+ enforce minimum spacing from our last send for this id
        if (s.lastTxMs && (now - s.lastTxMs) < minRetrySpacingMs) {
            s.nextAttemptMs = s.lastTxMs + minRetrySpacingMs;
        }
        //fw+ Enforce global DM concurrency cap: defer when issued >= cap in this pass
        if (s.isDM && dmSendsThisPass >= maxActiveDm) {
            // push next attempt slightly forward to yield to others
            s.nextAttemptMs = now + addJitter(busyRetryMs, dmJitterPct);
            ++it; continue;
        }
        //fw+ During adaptive initial grace, skip any action if destination was active very recently
        if (s.tries == 0 && (now - s.createdMs) < s.initialGraceMs) {
            // if destination active in last few seconds, keep waiting silently
            if (wasDestActiveRecently(s.to, 3000)) { ++it; continue; }
        }
        if (now < s.nextAttemptMs) { ++it; continue; }
        //fw+ Channel utilization gating: be less strict for DM deliveries, polite for broadcasts
#ifdef ARCH_PORTDUINO
        // In simulation, allow S&F DM deliveries regardless of channel util to validate E2E behavior
        bool txAllowed = true;
#else
        bool txAllowed = (!airTime) || (s.isDM ? airTime->isTxAllowedChannelUtil(false) : airTime->isTxAllowedChannelUtil(true));
#endif
        if (!txAllowed) {
            //fw+ reschedule when channel busy, but respect minimum spacing guard
            rescheduleAfterBusy(s);
            if (s.lastTxMs && s.nextAttemptMs < s.lastTxMs + minRetrySpacingMs) {
                s.nextAttemptMs = s.lastTxMs + minRetrySpacingMs;
            }
            LOG_DEBUG("fw+ S&F busy: defer id=0x%x next=%u ms", s.id, (unsigned)(s.nextAttemptMs - now));
            ++it; continue;
        }
        // Prepare payload from history for this id and to
        meshtastic_MeshPacket *p = nullptr;
        for (uint32_t i = 0; i < this->packetHistoryTotalCount; i++) {
            if (this->packetHistory[i].id == s.id) {
                if (isDelivered(s.id)) { p = nullptr; break; }
                //fw+ Early cancel: during soft grace window, if we already observed activity indicating native delivery
                // cancel schedule without sending anything. We detect by: destination is fresh and we recently saw
                // Routing/ACK or any reply from destination (proxied via isDestFresh + spacing as heuristic).
                if (now - s.createdMs < getScheduleSoftGraceMs()) {
                    if (isDestFresh(s.to)) {
                        // cancel schedule and clear history entry to avoid rescheduling
                        LOG_DEBUG("fw+ Soft-grace cancel id=0x%x dest=%u", s.id, (unsigned)s.to);
                        it = scheduleById.erase(it);
                        clearHistoryById(s.id);
                        break;
                    }
                }
                //fw+ Per-destination DM cap=1 and spacing guard just before send
                if (s.isDM) {
                    // Adaptive per-destination spacing
                    uint32_t spacing = computePerDestSpacingMs(s.to, s, now);
                    auto itLast = lastDestTxMs.find(s.to);
                    if (itLast != lastDestTxMs.end()) {
                        uint32_t last = itLast->second;
                        if (now - last < spacing) {
                            // defer to respect spacing
                            s.nextAttemptMs = last + spacing;
                            LOG_DEBUG("fw+ Per-dest spacing(adapt): defer id=0x%x to=%u next=%u ms", s.id, (unsigned)s.to, (unsigned)(s.nextAttemptMs - now));
                            p = nullptr;
                            break;
                        }
                    }
                    // Re-check freshness/route confidence right before allocate
                    if (!isDestFresh(s.to) || !hasSufficientRouteConfidence(s.to)) {
                        // Back off softly rather than fail immediately
                        s.nextAttemptMs = computeRetryDelayMs(s.tries ? s.tries : 1, estimateHops(s.to), s.lastTxMs, now) + addJitter(2000, dmJitterPct);
                        LOG_DEBUG("fw+ Recheck failed: defer id=0x%x dest=%u", s.id, (unsigned)s.to);
                        p = nullptr;
                        break;
                    }
                }
                p = allocDataPacket();
                if (!p) {
                    // fw+ DRAM pressure: back off in mini-server mode instead of risking instability
                    if (miniServerMode) {
                        rescheduleAfterBusy(s);
                        LOG_WARN("fw+ S&F mini-server: allocDataPacket failed, reschedule id=0x%x", s.id);
                        ++it; // continue outer loop
                    }
                    break;
                }
                p->to = s.to; // DM recipient or broadcast
                p->from = this->packetHistory[i].from;
                //fw+ Conditional forwarded-id: first try reuse original id; retries use fresh id + mapping
                uint32_t originalId = this->packetHistory[i].id;
                if (s.tries == 0) {
                    p->id = originalId; // try with original id first
                } else {
                    // allocDataPacket already generated a fresh id; remember mapping for ACK translation
                    rememberForwarded(p->id, originalId);
                }
                p->channel = this->packetHistory[i].channel;
                //fw+ Initialize hop limit consistently for forwards/retries
                if (s.isDM) {
                    // For DM, use default configured hop limit for reliability
                    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit); //fw+
                } else {
                    // For broadcast, preserve a modest hop limit
                    p->hop_limit = 2; //fw+ conservative rebroadcast distance
                }
                //fw+ ensure hop_start reflects a fresh transmission from this server
                p->hop_start = p->hop_limit;
                p->decoded.reply_id = this->packetHistory[i].reply_id;
                p->rx_time = this->packetHistory[i].time;
                p->decoded.emoji = (uint32_t)this->packetHistory[i].emoji;
                p->rx_rssi = this->packetHistory[i].rx_rssi;
                p->rx_snr = this->packetHistory[i].rx_snr;
                //fw+ For DM deliveries from S&F server, request ACK to allow schedule cancellation
                p->want_ack = s.isDM;
                //fw+ Preserve encryption: if history holds encrypted bytes, resend as encrypted frame
                if (this->packetHistory[i].encrypted) {
                    p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag; //fw+
                    p->decoded.portnum = (meshtastic_PortNum)0;                      //fw+
                    p->decoded.payload.size = 0;                                     //fw+
                    p->decoded.reply_id = 0;                                         //fw+
                    memcpy(p->encrypted.bytes, this->packetHistory[i].payload, this->packetHistory[i].payload_size); //fw+
                    p->encrypted.size = this->packetHistory[i].payload_size;                                         //fw+
                } else {
                    //fw+ Send plaintext as regular TEXT message (not S&F TEXT) to avoid APK chat spam
                    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
                    memcpy(p->decoded.payload.bytes, this->packetHistory[i].payload, this->packetHistory[i].payload_size);
                    p->decoded.payload.size = this->packetHistory[i].payload_size;
                }
                //fw+ Elevate priority for DM in mini-server to reduce contention with relays
                if (s.isDM && miniServerMode) {
                    p->priority = meshtastic_MeshPacket_Priority_HIGH;
                } else {
                    p->priority = meshtastic_MeshPacket_Priority_DEFAULT;
                }
                break;
            }
        }
        if (p) {
            //fw+ Just-in-time custody signaling before first transmission attempt
            if (s.tries == 0) {
                // Unicast CA to source to stop its retransmissions; broadcast CR for other S&F
                NodeNum src = 0, dst = 0; uint8_t ch = 0;
                if (getHistoryEndpoints(s.id, src, dst, ch)) {
                    // If encrypted path requested deferral, we may still be in soft grace; only send CA if grace elapsed
                    if (now - s.createdMs >= getDeferredCAGraceMs()) {
                        sendCustodyAck(src, s.id);
                    } else {
                        // still within CA grace: skip CA this time
                    }
                }
                sendCustodyClaim(s.id);
            }
            LOG_INFO("fw+ S&F deliver orig=0x%x fwd=0x%x try=%u (activeHist=%u)", s.id, p->id, (unsigned)s.tries + 1, (unsigned)countActiveHistory());
            service->sendToMesh(p);
            s.lastTxMs = nowMs();
            if (s.isDM && dmSendsThisPass < 0xFFFFFFFF) dmSendsThisPass++;
            if (s.isDM) lastDestTxMs[s.to] = s.lastTxMs;
            // Keep DM scheduled; cancel on ACK (via ReliableRouter hook). Broadcasts are one-shot.
            if (!s.isDM) {
                //fw+ Immediately clear broadcast history to free DRAM in mini-server mode
                if (miniServerMode) {
                    clearHistoryById(s.id);
                }
                it = scheduleById.erase(it);
                continue;
            }
            else {
                //fw+ no user-visible notification; protocol-level ACK only
            }
        }
        // reschedule/backoff or remove
        s.tries++;
        if (!s.isDM) { it = scheduleById.erase(it); continue; }
        if (s.tries >= dmMaxTries) {
            //fw+ terminal failure: emit DF and markFailed; drop schedule
            broadcastDeliveryFailedControl(s.id, (uint32_t)meshtastic_Routing_Error_NO_ROUTE);
            markFailed(s.id);
            //fw+ Start per-destination cooldown after terminal DM failure
            startDestCooldown(s.to);
            clearHistoryById(s.id);
            it = scheduleById.erase(it);
            continue;
        }
        //fw+ DM pacing: >=60s + hop scaling, guard spacing and overall deadline
        uint8_t estHops = estimateHops(s.to);
        uint32_t target = computeRetryDelayMs(s.tries, estHops, s.lastTxMs, now);
        if (s.deadlineMs && target > s.deadlineMs) {
            // terminal: emit DF, markFailed and drop
            broadcastDeliveryFailedControl(s.id, (uint32_t)meshtastic_Routing_Error_NO_ROUTE);
            markFailed(s.id);
            startDestCooldown(s.to);
            clearHistoryById(s.id);
            it = scheduleById.erase(it);
            continue;
        }
        s.nextAttemptMs = target;
        LOG_DEBUG("fw+ S&F reschedule id=0x%x in %u ms (try=%u)", s.id, (unsigned)(s.nextAttemptMs - now), s.tries);
        ++it;
    }
}

//fw+ Adaptive per-destination spacing computation (hops, density, chanutil, peers, TTL, queue depth)
uint32_t StoreForwardModule::computePerDestSpacingMs(NodeNum dest, const CustodySchedule &s, uint32_t now) const
{
    // Base: 20s + 5s per estimated hop
    uint8_t hops = estimateHops(dest);
    uint32_t base = 20000 + (uint32_t)hops * 5000;
    // Density bump
    if (isDenseEnvironment()) base += 15000;
    // Channel utilization clamp
    if (airTime && airTime->max_channel_util_percent >= 40) {
        if (base < 60000) base = 60000;
    }
    // Recent FW+ peers → small extra
    if (hasRecentSfPeers(12 * 60 * 60 * 1000UL)) base += 5000;
    // Queue depth to same dest → minimum 30s
    uint32_t sameDestQueued = 0;
    for (const auto &kv : scheduleById) if (kv.second.isDM && kv.second.to == dest) sameDestQueued++;
    if (sameDestQueued > 1 && base < 30000) base = 30000;
    // TTL budget: try to spread remaining attempts
    if (s.deadlineMs > now) {
        uint32_t remaining = s.deadlineMs - now;
        uint32_t triesLeft = (dmMaxTries > s.tries ? (uint32_t)(dmMaxTries - s.tries) : 1u);
        uint32_t perTry = (triesLeft ? remaining / triesLeft : remaining);
        if (perTry > 5000) perTry -= 5000; // leave 5s guard
        if (base > perTry) base = perTry;  // clamp to TTL budget
    }
    return addJitter(base, dmJitterPct);
}

//fw+ Compute adaptive grace window before S&F acts, based on hops, channel util, density, and signal quality
uint32_t StoreForwardModule::computeAdaptiveGraceMs(NodeNum dest, const PacketHistoryStruct &rec) const
{
    // Base 8s .. 25s window depending on conditions
    uint32_t base = 8000;
    uint8_t hops = estimateHops(dest);
    // +2s per hop
    base += (uint32_t)hops * 2000;
    // Channel utilization bump
    if (airTime) {
        // up to +8s for high util
        uint32_t util = airTime->max_channel_util_percent;
        if (util >= 40) base += 8000; else if (util >= 25) base += 4000; else if (util >= 15) base += 2000;
    }
    // Signal quality bump (low SNR -> more grace)
    if (rec.rx_snr < 5.0f) base += 3000; else if (rec.rx_snr < 10.0f) base += 1500;
    // Dense mesh bump
    if (isDenseEnvironment()) base += 5000;
    // Clamp to 8..25s and add jitter
    if (base < 8000) base = 8000; if (base > 25000) base = 25000;
    return addJitter(base, dmJitterPct);
}

//fw+ Detect if destination was active very recently (received by us)
bool StoreForwardModule::wasDestActiveRecently(NodeNum dest, uint32_t recentMs) const
{
    if (!nodeDB) return false;
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(dest);
    if (!n) return false;
    uint32_t now = getValidTime(RTCQualityFromNet);
    if (n->last_heard == 0 || now == 0) return false;
    return (now - n->last_heard) * 1000UL <= recentMs;
}
//fw+ Locate endpoints for a stored id
bool StoreForwardModule::getHistoryEndpoints(uint32_t id, NodeNum &src, NodeNum &dst, uint8_t &channel)
{
    for (int32_t i = (int32_t)this->packetHistoryTotalCount - 1; i >= 0; --i) {
        if (this->packetHistory[i].id == id) {
            src = this->packetHistory[i].from;
            dst = this->packetHistory[i].to;
            channel = this->packetHistory[i].channel;
            return true;
        }
    }
    return false;
}

//fw+ Check that we have a fresh enough last_heard for destination
bool StoreForwardModule::isDestFresh(NodeNum dest) const
{
    meshtastic_NodeInfoLite *n = nodeDB ? nodeDB->getMeshNode(dest) : nullptr;
    if (!n) return false;
    uint32_t now = getValidTime(RTCQualityFromNet);
    if (n->last_heard == 0 || now == 0) return false;
    //fw+ use dynamic allowance based on mesh density/quiet
    return (now - n->last_heard) <= getDestStaleAllowance();
}

//fw+ In dense/active meshes keep 30 min; in sparse/quiet allow up to 2h
uint32_t StoreForwardModule::getDestStaleAllowance() const
{
    bool dense = isDenseEnvironment();
    if (dense) return destStaleSeconds; // default 30min
    // sparse/quiet: extend to 2h (7200s)
    return (uint32_t)7200;
}

//fw+ Minimal DV-ETX confidence gating (uses NextHopRouter snapshot if available)
bool StoreForwardModule::hasSufficientRouteConfidence(NodeNum dest) const
{
    // Use virtual hook on Router (no RTTI)
    if (!router) return true;
    return router->hasRouteConfidence(dest, minRouteConfidence);
}

//fw+ user-visible notifications removed; rely on protocol-level ACK only

void StoreForwardModule::rescheduleAfterBusy(CustodySchedule &s)
{
    uint32_t delay = addJitter(busyRetryMs, dmJitterPct);
    s.nextAttemptMs = nowMs() + delay;
}

StoreForwardModule::StoreForwardModule()
    : concurrency::OSThread("StoreForward"),
      ProtobufModule("StoreForward", meshtastic_PortNum_STORE_FORWARD_APP, &meshtastic_StoreAndForward_msg)
{
    //fw+ accept encrypted packets for opaque custody
    encryptedOk = true;

#if defined(ARCH_ESP32) || defined(ARCH_PORTDUINO)

    isPromiscuous = true;

    if (StoreForward_Dev) {
        moduleConfig.store_forward.enabled = 1;
    }

    //fw+ Respect user setting: do not auto-enable here; role-based defaults are applied on role change

    //fw+ Respect persisted user settings for custody signals; do not normalize here

    //fw+ Do NOT auto-enable based on role. Only explicit flags control init.
    if (moduleConfig.store_forward.is_server) {
        LOG_INFO("Init Store & Forward Module in Server mode");
#if defined(ARCH_ESP32)
        if (memGet.getPsramSize() > 0 && memGet.getFreePsram() >= 1024 * 1024) {
            // Do the startup here
            if (moduleConfig.store_forward.history_return_max)
                this->historyReturnMax = moduleConfig.store_forward.history_return_max;
            if (moduleConfig.store_forward.history_return_window)
                this->historyReturnWindow = moduleConfig.store_forward.history_return_window;
            if (moduleConfig.store_forward.records)
                this->records = moduleConfig.store_forward.records;
            this->heartbeat = moduleConfig.store_forward.heartbeat ? moduleConfig.store_forward.heartbeat : false;
            this->populatePSRAM();
            is_server = true;
        } else {
            // fw+ Mini-server mode without PSRAM when user requested server (is_server true)
            // Conservative defaults to avoid DRAM exhaustion
            // fw+ Tiny DRAM buffer with upper clamp for safety
            if (!moduleConfig.store_forward.records) {
                this->records = 16; // fw+ ultra-small DRAM buffer for mini-server
            } else {
                uint32_t r = moduleConfig.store_forward.records; // fw+
                this->records = (r > 64 ? 64 : r);              // fw+ clamp to 64 max in mini-server
            }
            if (moduleConfig.store_forward.history_return_max)
                this->historyReturnMax = moduleConfig.store_forward.history_return_max;
            else
                this->historyReturnMax = 8; // fw+ minimal replay batch
            if (moduleConfig.store_forward.history_return_window)
                this->historyReturnWindow = moduleConfig.store_forward.history_return_window;
            else
                this->historyReturnWindow = 30; // fw+ 30 minutes
            this->heartbeat = false; // fw+ reduce CPU/RAM pressure

            // Allocate in DRAM instead of PSRAM
            this->packetHistory = static_cast<PacketHistoryStruct *>(calloc(this->records, sizeof(PacketHistoryStruct)));
            if (!this->packetHistory) {
                LOG_ERROR("fw+ S&F mini-server: DRAM alloc failed, disabling S&F server");
            } else {
                miniServerMode = true; // fw+
                is_server = true;
                LOG_WARN("fw+ S&F mini-server active (no PSRAM): records=%u returnMax=%u window=%u min",
                         (unsigned)this->records, (unsigned)this->historyReturnMax, (unsigned)this->historyReturnWindow);
            }
        }
#elif defined(ARCH_PORTDUINO)
        //fw+ Allow server mode without PSRAM on Portduino; use small RAM buffer
        if (!moduleConfig.store_forward.records)
            this->records = 128; // small default for simulation
        if (moduleConfig.store_forward.history_return_max)
            this->historyReturnMax = moduleConfig.store_forward.history_return_max;
        if (moduleConfig.store_forward.history_return_window)
            this->historyReturnWindow = moduleConfig.store_forward.history_return_window;
        this->heartbeat = moduleConfig.store_forward.heartbeat ? moduleConfig.store_forward.heartbeat : false;
        this->populatePSRAM();
        is_server = true;
#endif
    } else if (moduleConfig.store_forward.enabled) {
        // Client only when explicitly enabled
        is_client = true;
        LOG_INFO("Init Store & Forward Module in Client mode");
    } else {
        disable();
    }
#endif

#ifdef ARCH_NRF52
    //fw+ Client-only S&F on NRF52: no PSRAM, no server; enable client handler paths
    if (moduleConfig.store_forward.enabled) {
        is_client = true;
        is_server = false;
        LOG_INFO("Init Store & Forward Module in Client mode (NRF52)");
    } else {
        disable();
    }
#endif
}

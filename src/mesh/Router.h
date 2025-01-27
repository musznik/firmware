#pragma once

#include "Channels.h"
#include "MemoryPool.h"
#include "MeshTypes.h"
#include "Observer.h"
#include "PointerQueue.h"
#include "RadioInterface.h"
#include "concurrency/OSThread.h"
#include "PacketCounter.h"
/**
 * A mesh aware router that supports multiple interfaces.
 */

class Router : protected concurrency::OSThread
{
  private:
    /// Packets which have just arrived from the radio, ready to be processed by this service and possibly
    /// forwarded to the phone.
    PointerQueue<meshtastic_MeshPacket> fromRadioQueue;

  protected:
    RadioInterface *iface = NULL;

  public:
    /**
     * Constructor
     *
     */
    Router();
 
    /**
     * Currently we only allow one interface, that may change in the future
     */
    void addInterface(RadioInterface *_iface) { iface = _iface; }

    /**
     * do idle processing
     * Mostly looking in our incoming rxPacket queue and calling handleReceived.
     */
    virtual int32_t runOnce() override;

    /**
     * Works like send, but if we are sending to the local node, we directly put the message in the receive queue.
     * This is the primary method used for sending packets, because it handles both the remote and local cases.
     *
     * NOTE: This method will free the provided packet (even if we return an error code)
     */
    ErrorCode sendLocal(meshtastic_MeshPacket *p, RxSource src = RX_SRC_RADIO);

    /** Attempt to cancel a previously sent packet.  Returns true if a packet was found we could cancel */
    bool cancelSending(NodeNum from, PacketId id);

    /** Allocate and return a meshpacket which defaults as send to broadcast from the current node.
     * The returned packet is guaranteed to have a unique packet ID already assigned
     */
    meshtastic_MeshPacket *allocForSending();

    /** Return Underlying interface's TX queue status */
    meshtastic_QueueStatus getQueueStatus();
    
    /**
     * @return our local nodenum */
    NodeNum getNodeNum();

    /** Wake up the router thread ASAP, because we just queued a message for it.
     * FIXME, this is kinda a hack because we don't have a nice way yet to say 'wake us because we are 'blocked on this queue'
     */
    void setReceivedMessage();


    /**
     * RadioInterface calls this to queue up packets that have been received from the radio.  The router is now responsible for
     * freeing the packet
     */
    virtual void enqueueReceivedMessage(meshtastic_MeshPacket *p);

    /**
     * Send a packet on a suitable interface.  This routine will
     * later free() the packet to pool.  This routine is not allowed to stall.
     * If the txmit queue is full it might return an error
     *
     * NOTE: This method will free the provided packet (even if we return an error code)
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p);

    /* Statistics for the amount of duplicate received packets and the amount of times we cancel a relay because someone did it
        before us */
    uint32_t rxDupe = 0, txRelayCanceled = 0;

  protected:
    friend class RoutingModule;

    /**
     * Should this incoming filter be dropped?
     *
     * FIXME, move this into the new RoutingModule and do the filtering there using the regular module logic
     *
     * Called immediately on reception, before any further processing.
     * @return true to abandon the packet
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) { return false; }

    /**
     * Every (non duplicate) packet this node receives will be passed through this method.  This allows subclasses to
     * update routing tables etc... based on what we overhear (even for messages not destined to our node)
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c);
    
    /**
     * Send an ack or a nak packet back towards whoever sent idFrom
     */
    void sendAckNak(meshtastic_Routing_Error err, NodeNum to, PacketId idFrom, ChannelIndex chIndex, uint8_t hopLimit = 0);

  private:
    /**
     * Called from loop()
     * Handle any packet that is received by an interface on this node.
     * Note: some packets may merely being passed through this node and will be forwarded elsewhere.
     *
     * Note: this packet will never be called for messages sent/generated by this node.
     * Note: this method will free the provided packet.
     */
    void perhapsHandleReceived(meshtastic_MeshPacket *p);

    /**
     * Called from perhapsHandleReceived() - allows subclass message delivery behavior.
     * Handle any packet that is received by an interface on this node.
     * Note: some packets may merely being passed through this node and will be forwarded elsewhere.
     *
     * Note: this packet will never be called for messages sent/generated by this node.
     * Note: this method will free the provided packet.
     */
    void handleReceived(meshtastic_MeshPacket *p, RxSource src = RX_SRC_RADIO);

    /** Frees the provided packet, and generates a NAK indicating the specifed error while sending */
    void abortSendAndNak(meshtastic_Routing_Error err, meshtastic_MeshPacket *p);
    /* packet counter history */
     PacketCounter m_packetCounter; 
};

/** FIXME - move this into a mesh packet class
 * Remove any encryption and decode the protobufs inside this packet (if necessary).
 *
 * @return true for success, false for corrupt packet.
 */
bool perhapsDecode(meshtastic_MeshPacket *p);

/** Return 0 for success or a Routing_Error code for failure
 */
meshtastic_Routing_Error perhapsEncode(meshtastic_MeshPacket *p);

extern Router *router;

/// Generate a unique packet id
// FIXME, move this someplace better
PacketId generatePacketId();

#define BITFIELD_WANT_RESPONSE_SHIFT 1
#define BITFIELD_OK_TO_MQTT_SHIFT 0
#define BITFIELD_WANT_RESPONSE_MASK (1 << BITFIELD_WANT_RESPONSE_SHIFT)
#define BITFIELD_OK_TO_MQTT_MASK (1 << BITFIELD_OK_TO_MQTT_SHIFT)
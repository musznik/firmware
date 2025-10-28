#pragma once
//fw+ RF Bridge Protocol - Direct radio simulation bypassing PhoneAPI
// Purpose: Minimal binary protocol for fast packet forwarding between firmware and Python RF simulator
// Architecture: Unix domain socket (fast IPC, no TCP overhead)

#include <stdint.h>

// Maximum payload size for RF packets (same as MeshPacket encrypted)
#define RF_BRIDGE_MAX_PAYLOAD 512

// Message types
enum RFBridgeMessageType : uint8_t {
    RF_MSG_PACKET_TX = 1,    // Firmware → Bridge: Transmit packet to mesh
    RF_MSG_PACKET_RX = 2,    // Bridge → Firmware: Receive packet from mesh
    RF_MSG_HEARTBEAT = 3,    // Keepalive (both directions)
};

// Packed struct for minimal overhead (no padding)
#pragma pack(push, 1)

// TX message: Firmware sends to bridge for radio simulation
struct RFBridgeTxPacket {
    uint8_t type;           // RF_MSG_PACKET_TX
    uint32_t nodeNum;       // Transmitting node (for bridge routing)
    uint32_t packetId;      // Packet ID for dedup and tracking
    uint32_t from;          // Sender node number
    uint32_t to;            // Destination (unicast or broadcast)
    uint8_t hopLimit;       // Remaining hops
    uint8_t hopStart;       // Original hop limit (for rebroadcast detection)
    uint8_t channel;        // Channel hash
    uint8_t priority;       // Packet priority
    uint8_t wantAck;        // Want acknowledgment flag (for reliable delivery)
    uint8_t viaMqtt;        // Packet came via MQTT (for uplink filtering)
    uint8_t pkiEncrypted;   // Packet is encrypted using PKI (not PSK)
    uint8_t delayed;        // Delayed broadcast type (0=none, 1=direct, 2=default)
    uint8_t publicKeyLen;   // Public key length (0 if not present, max 32)
    uint8_t publicKey[32];  // Public key used for encryption (if PKI)
    uint16_t payloadLen;    // Encrypted payload length
    uint8_t payload[RF_BRIDGE_MAX_PAYLOAD]; // Encrypted payload (opaque to bridge)
};

// RX message: Bridge sends to firmware after RF simulation
struct RFBridgeRxPacket {
    uint8_t type;           // RF_MSG_PACKET_RX
    uint32_t nodeNum;       // Receiving node (for bridge routing)
    uint32_t packetId;      // Packet ID
    uint32_t from;          // Sender node number
    uint32_t to;            // Destination
    uint8_t hopLimit;       // Remaining hops (decremented by rebroadcasts)
    uint8_t hopStart;       // Original hop limit
    uint8_t channel;        // Channel hash
    uint8_t priority;       // Packet priority
    uint8_t wantAck;        // Want acknowledgment flag (for reliable delivery)
    uint8_t viaMqtt;        // Packet came via MQTT (for uplink filtering)
    uint8_t pkiEncrypted;   // Packet is encrypted using PKI (not PSK)
    uint8_t delayed;        // Delayed broadcast type (0=none, 1=direct, 2=default)
    uint8_t publicKeyLen;   // Public key length (0 if not present, max 32)
    uint8_t publicKey[32];  // Public key used for encryption (if PKI)
    uint32_t relayNode;     // Last relay node (for hop tracking)
    int16_t rxRssi;         // Received signal strength (dBm)
    float rxSnr;            // Signal-to-noise ratio (dB)
    uint16_t payloadLen;    // Encrypted payload length
    uint8_t payload[RF_BRIDGE_MAX_PAYLOAD]; // Encrypted payload
};

// Heartbeat message (minimal, keepalive only)
struct RFBridgeHeartbeat {
    uint8_t type;           // RF_MSG_HEARTBEAT
    uint32_t nodeNum;       // Node number
    uint32_t timestamp;     // Unix timestamp
};

#pragma pack(pop)

// Helper: Get message type from buffer
inline RFBridgeMessageType getRFBridgeMessageType(const uint8_t *buf) {
    return (RFBridgeMessageType)buf[0];
}


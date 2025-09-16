<div markdown="1">

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware+</h1>
# Alternative Meshtastic Firmware+

![GitHub release downloads](https://img.shields.io/github/downloads/meshtastic/firmware/total)
[![CI](https://img.shields.io/github/actions/workflow/status/meshtastic/firmware/main_matrix.yml?branch=master&label=actions&logo=github&color=yellow)](https://github.com/meshtastic/firmware/actions/workflows/ci.yml)
[![CLA assistant](https://cla-assistant.io/readme/badge/meshtastic/firmware)](https://cla-assistant.io/meshtastic/firmware)
[![Fiscal Contributors](https://opencollective.com/meshtastic/tiers/badge.svg?label=Fiscal%20Contributors&color=deeppink)](https://opencollective.com/meshtastic/)
[![Vercel](https://img.shields.io/static/v1?label=Powered%20by&message=Vercel&style=flat&logo=vercel&color=000000)](https://vercel.com?utm_source=meshtastic&utm_campaign=oss)

## Fork features

- Protect private messages on public MQTT: This commit introduces a feature that enhances security by preventing the sending of private messages between nodes when the MQTT uplink is enabled. The motivation behind this change is to ensure that sensitive information is not inadvertently transmitted over potentially insecure channels. Public messages (broadcast) along with telemetry data, remain unrestricted to maintain the core functionality of the network. This feature is configurable via the APK+ for android, providing users with flexibility and control over their network's security. Default: True.
- Default hop limit changed from 3 to 5.
- Allow to administrate node with "Managed Mode" via Wifi or BT.
- Less air limits. (configurable)
- Support for private text message "nodes" (only online) or "nodes all" and return list of nodes.
- Allow to change node position over mesh.
- Local stats over mesh (additional telemetry).
- Sniffer mode, transfers to the connected device (TCP or BT) packets that are not public and do not belong to us without decrypting them. This allows for a better understanding of network operation. Default OFF. Use APK+ to enable it.
- Router mode always passes everything, not just CORE_PORTS (NodeInfo, Text, Position, Telemetry, Routing) by default.
- Neighbor Info enabled again on primary channel (LongFast, MediumFast, etc.), enabled by default.
- Node status custom text transmitted over mesh (as new protobuf, port: 278), requires custom APK like "Meshtastic Android APK+ (loranet version)" for operations.
- Handling of OnDemand packages, the ability to retrieve packet reception statistics from a node in 10-minute intervals for 40 measurements (6 hours). The ability to request a list of nodes seen by the node (2 hours). Supported in APK+. Port 354.
- Transmitting the average number of received packets along with 6 measurements (1 hour) for every 10 minutes over the mesh.
- AutoResponder with user defined text
- AutoRedirector for message redirection to another node
- OnDemand requests (new protobuf)
   - total number of reads and writes (RX/RX) for a 10-minute window across 40 measurements (6h~)
   - average time spent reading (RX) a packet by LoRA
   - usage counter of a given protobuf (port) since the node was started
   - remote node list request
   - last 10 routing logs (from,to,type)
   - air activity for tx/rx/idle/rxnoise (10min window) in 10 measurments
   - routing table for NextHop
   - simple ping request (ack & non-ack)
   -request node stats (local stats, telemetry, additional stats)
- Signal Reply Module (write "ping" to node). https://github.com/VilemR/meshtstic_modules_mod
- DV-ETX routing metric for path selection: uses Expected Transmission Count to score links and prefer higher-quality routes across the mesh. Configurable and interoperable with existing routing logic.
- Opportunistic rebroadcast: probabilistic, condition-aware rebroadcasting to improve reach in sparse or lossy topologies while minimizing duplicate traffic and airtime usage.
- Telemetry limiter: adaptive throttling and aggregation of telemetry ports to respect airtime budgets; configurable limits via APK+ while preserving essential health data.
- Position limiter: rate limits and jitters GPS position broadcasts; supports movement thresholds (distance/speed) to avoid unnecessary updates when stationary.
- Passive next-hop learning: NextHopRouter observes traffic to learn and update next-hop mappings without active probes, improving convergence and reducing overhead.
- Active Store & Forward: buffers messages for temporarily unreachable nodes and forwards them when peers reappear; includes TTL and de-dup safeguards.


## Overview

This repository contains the official device firmware for Meshtastic, an open-source LoRa mesh networking project designed for long-range, low-power communication without relying on internet or cellular infrastructure. The firmware supports various hardware platforms, including ESP32, nRF52, RP2040/RP2350, and Linux-based devices.

Meshtastic enables text messaging, location sharing, and telemetry over a decentralized mesh network, making it ideal for outdoor adventures, emergency preparedness, and remote operations.

### Get Started

- 🔧 **[Building Instructions](https://meshtastic.org/docs/development/firmware/build)** – Learn how to compile the firmware from source.
- ⚡ **[Flashing Instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)** – Install or update the firmware on your device.

Join our community and help improve Meshtastic! 🚀

## Stats

![Alt](https://repobeats.axiom.co/api/embed/8025e56c482ec63541593cc5bd322c19d5c0bdcf.svg "Repobeats analytics image")


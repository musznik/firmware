# Alternative Meshtastic Firmware+

![GitHub release downloads](https://img.shields.io/github/downloads/meshtastic/firmware/total)
[![CI](https://img.shields.io/github/actions/workflow/status/meshtastic/firmware/main_matrix.yml?branch=master&label=actions&logo=github&color=yellow)](https://github.com/meshtastic/firmware/actions/workflows/ci.yml)
[![CLA assistant](https://cla-assistant.io/readme/badge/meshtastic/firmware)](https://cla-assistant.io/meshtastic/firmware)
[![Fiscal Contributors](https://opencollective.com/meshtastic/tiers/badge.svg?label=Fiscal%20Contributors&color=deeppink)](https://opencollective.com/meshtastic/)
[![Vercel](https://img.shields.io/static/v1?label=Powered%20by&message=Vercel&style=flat&logo=vercel&color=000000)](https://vercel.com?utm_source=meshtastic&utm_campaign=oss)

## Fork features

- Protect private messages on public MQTT: This commit introduces a feature that enhances security by preventing the sending of private messages between nodes when the MQTT uplink is enabled. The motivation behind this change is to ensure that sensitive information is not inadvertently transmitted over potentially insecure channels. Public messages broadcast to address 0xffffffff, along with telemetry data, remain unrestricted to maintain the core functionality of the network. This feature is configurable via the command **meshtastic --set mqtt.secure_messages [true|false]** providing users with flexibility and control over their network's security. Default: True
- Default hop limit changed from 3 to 5.
- Allow to administrate node with "Managed Mode" via Wifi or BT.
- Less air limits.
- Support for private text message "nodes" (only online) or "nodes all" and return list of nodes.
- Local stats over mesh (additional telemetry).
- Sniffer mode, transfers to the connected device (TCP or BT) packets that are not public and do not belong to us without decrypting them. This allows for a better understanding of network operation.
- Router mode always passes everything, not just CORE_PORTS (NodeInfo, Text, Position, Telemetry, Routing) by default.
- Neighbor Info enabled again on primary channel (LongFast, MediumFast, etc.), enabled by default.
- Node status custom text transmitted over mesh (as new protobuf, port: 278), requires custom APK like "Meshtastic Android APK+" for operations

## Overview

This repository contains the device firmware for the Meshtastic project.

- **[Building Instructions](https://meshtastic.org/docs/development/firmware/build)**
- **[Flashing Instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)**

## Stats

![Alt](https://repobeats.axiom.co/api/embed/a92f097d9197ae853e780ec53d7d126e545629ab.svg "Repobeats analytics image")
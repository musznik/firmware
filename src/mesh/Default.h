#pragma once
#include <MeshRadio.h>
#include <NodeDB.h>
#include <RadioInterface.h>
#include <cmath>
#include <cstdint>
#include <meshUtils.h>
#define ONE_DAY 24 * 60 * 60
#define ONE_MINUTE_MS 60 * 1000
#define THIRTY_SECONDS_MS 30 * 1000
#define TWO_SECONDS_MS 2 * 1000
#define FIVE_SECONDS_MS 5 * 1000
#define FIFTEEN_SECONDS_MS 15 * 1000
#define TEN_SECONDS_MS 10 * 1000
#define MAX_INTERVAL INT32_MAX // FIXME: INT32_MAX to avoid overflow issues with Apple clients but should be UINT32_MAX

#define min_default_telemetry_interval_secs 3 * 60 * 60 // 2h
#define default_gps_update_interval IF_ROUTER(ONE_DAY, 2 * 60)
#define default_telemetry_broadcast_interval_secs IF_ROUTER(ONE_DAY / 2, 60 * 60)
#define default_broadcast_interval_secs IF_ROUTER(ONE_DAY / 2, 60 * 60)
#define default_broadcast_smart_minimum_interval_secs 5 * 60
#define min_default_broadcast_interval_secs 3 * 60 * 60 // 2h
#define min_default_broadcast_smart_minimum_interval_secs 5 * 60
#define default_wait_bluetooth_secs IF_ROUTER(1, 60)
#define default_sds_secs IF_ROUTER(ONE_DAY, UINT32_MAX) // Default to forever super deep sleep
#define default_ls_secs IF_ROUTER(ONE_DAY, 5 * 60)
#define default_min_wake_secs 10
#define default_screen_on_secs IF_ROUTER(1, 60 * 10)
#define default_node_info_broadcast_secs 3 * 60 * 60
#define default_neighbor_info_broadcast_secs 5 * 60 * 60
#define min_node_info_broadcast_secs 30 * 60 // No regular broadcasts of more than once an hour
#define min_neighbor_info_broadcast_secs 2 * 60 * 60
#define default_map_publish_interval_secs 60 * 60
#ifdef USERPREFS_RINGTONE_NAG_SECS
#define default_ringtone_nag_secs USERPREFS_RINGTONE_NAG_SECS
#else
#define default_ringtone_nag_secs 15
#endif
#define default_network_ipv6_enabled false

#define default_mqtt_address "loranet.pl"
#define default_mqtt_username ""
#define default_mqtt_password ""
#define default_mqtt_root "msh/PL"
#define default_do_not_send_prvate_messages_over_mqtt true
#define default_sniffer_enabled false
#define default_local_stats_over_mesh_enabled true
#define default_local_stats_extended_over_mesh_enabled true
#define default_idlegame_enabled false
#define default_chanutil_user_additional 10
#define default_chantxutil_user_additional 5
#define default_autoresponder_enabled false
#define default_autoredirect_messages_enabled false
#define default_mqtt_encryption_enabled true
#define default_mqtt_tls_enabled false

#define IF_ROUTER(routerVal, normalVal)                                                                                          \
    ((config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||                                                        \
      config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)                                                     \
         ? (routerVal)                                                                                                           \
         : (normalVal))

class Default
{
  public:
    static uint32_t getConfiguredOrDefaultMs(uint32_t configuredInterval);
    static uint32_t getConfiguredOrDefaultMs(uint32_t configuredInterval, uint32_t defaultInterval);
    static uint32_t getConfiguredOrDefault(uint32_t configured, uint32_t defaultValue);
    // Note: numOnlineNodes uses uint32_t to match the public API and allow flexibility,
    // even though internal node counts use uint16_t (max 65535 nodes)
    static uint32_t getConfiguredOrDefaultMsScaled(uint32_t configured, uint32_t defaultValue, uint32_t numOnlineNodes);
    static uint32_t getLightlyScaledWindowMs(uint32_t baseSeconds, uint32_t numOnlineNodes, bool includeJitter = true);
    static uint8_t getConfiguredOrDefaultHopLimit(uint8_t configured);
    static uint32_t getConfiguredOrMinimumValue(uint32_t configured, uint32_t minValue);

  private:
    // Note: Kept as uint32_t to match the public API parameter type
    static float congestionScalingCoefficient(uint32_t numOnlineNodes)
    {
        if (numOnlineNodes <= 40) {
            return 1.0;
        } else {
            // Resolve SF and BW from preset or manual config
            // When use_preset is true, config.lora.spread_factor and bandwidth may be 0
            // because applyModemConfig() sets them on RadioInterface, not on config.lora
            float bwKHz;
            uint8_t sf;
            uint8_t cr;
            if (config.lora.use_preset) {
                modemPresetToParams(config.lora.modem_preset, false, bwKHz, sf, cr);
            } else {
                sf = config.lora.spread_factor;
                bwKHz = bwCodeToKHz(config.lora.bandwidth);
            }

            // Guard against invalid values
            sf = clampSpreadFactor(sf);
            bwKHz = clampBandwidthKHz(bwKHz);

            // throttlingFactor = 2^SF / (BW_in_kHz * scaling_divisor)
            // With scaling_divisor=100:
            // In SF11 and BW=250khz (longfast), this gives 0.08192 rather than the original 0.075
            // In SF10 and BW=250khz (mediumslow), this gives 0.04096 rather than the original 0.04
            // In SF9 and BW=250khz (mediumfast), this gives 0.02048 rather than the original 0.02
            // In SF7 and BW=250khz (shortfast), this gives 0.00512 rather than the original 0.01
            float throttlingFactor = static_cast<float>(pow_of_2(sf)) / (bwKHz * 100.0f);

#if USERPREFS_EVENT_MODE
            // If we are in event mode, scale down the throttling factor by 4
            throttlingFactor = static_cast<float>(pow_of_2(sf)) / (bwKHz * 25.0f);
#endif

            // Scaling up traffic based on number of nodes over 40
            int nodesOverForty = (numOnlineNodes - 40);
            return 1.0 + (nodesOverForty * throttlingFactor); // Each number of online node scales by throttle factor
        }
    }

    static float congestionScalingCoefficientLight(int numOnlineNodes)
    {
        // Lighter scaling than the default method for intra-telemetry windows.
        if (numOnlineNodes <= 10) {
            return 0.8;
        } else if (numOnlineNodes <= 20) {
            return 0.9;
        } else if (numOnlineNodes <= 40) {
            return 1.0;
        } else {
            float throttlingFactor = 0.02; // much lighter than default 0.075
            if (config.lora.use_preset && config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW)
                throttlingFactor = 0.03;
            else if (config.lora.use_preset && config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST)
                throttlingFactor = 0.015;
            else if (config.lora.use_preset &&
                     IS_ONE_OF(config.lora.modem_preset, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,
                               meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO,
                               meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW))
                throttlingFactor = 0.01;

#if USERPREFS_EVENT_MODE
            throttlingFactor = 0.015;
#endif

            int nodesOverForty = (numOnlineNodes - 40);
            return 1.0 + (nodesOverForty * throttlingFactor);
        }
    }
};
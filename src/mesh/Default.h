#pragma once
#include <NodeDB.h>
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

#define min_default_telemetry_interval_secs 10 * 60 // 10 min
#define default_gps_update_interval IF_ROUTER(ONE_DAY, 2 * 60)
#define default_telemetry_broadcast_interval_secs IF_ROUTER(60 * 60 * 3, 60 * 60)
#define default_broadcast_interval_secs IF_ROUTER(60 * 60, 60 * 60)
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
#define default_ringtone_nag_secs 60
#endif

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
    ((config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER) ? (routerVal) : (normalVal))

class Default
{
  public:
    static uint32_t getConfiguredOrDefaultMs(uint32_t configuredInterval);
    static uint32_t getConfiguredOrDefaultMs(uint32_t configuredInterval, uint32_t defaultInterval);
    static uint32_t getConfiguredOrDefault(uint32_t configured, uint32_t defaultValue);
    static uint32_t getConfiguredOrDefaultMsScaled(uint32_t configured, uint32_t defaultValue, uint32_t numOnlineNodes);
    static uint32_t getLightlyScaledWindowMs(uint32_t baseSeconds, uint32_t numOnlineNodes, bool includeJitter = true);
    static uint8_t getConfiguredOrDefaultHopLimit(uint8_t configured);
    static uint32_t getConfiguredOrMinimumValue(uint32_t configured, uint32_t minValue);

  private:
    static float congestionScalingCoefficient(int numOnlineNodes)
    {
        // Increase frequency of broadcasts for small networks regardless of preset
        if (numOnlineNodes <= 10) {
            return 0.6;
        } else if (numOnlineNodes <= 20) {
            return 0.7;
        } else if (numOnlineNodes <= 30) {
            return 0.8;
        } else if (numOnlineNodes <= 40) {
            return 1.0;
        } else {
            float throttlingFactor = 0.075;
            if (config.lora.use_preset && config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW)
                throttlingFactor = 0.04;
            else if (config.lora.use_preset && config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST)
                throttlingFactor = 0.02;
            else if (config.lora.use_preset &&
                     IS_ONE_OF(config.lora.modem_preset, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,
                               meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO,
                               meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW))
                throttlingFactor = 0.01;

#if USERPREFS_EVENT_MODE
            // If we are in event mode, scale down the throttling factor
            throttlingFactor = 0.04;
#endif

            // Scaling up traffic based on number of nodes over 40
            int nodesOverForty = (numOnlineNodes - 40);
            return 1.0 + (nodesOverForty * throttlingFactor); // Each number of online node scales by 0.075 (default)
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
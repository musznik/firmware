#include "Default.h"

#include "meshUtils.h"

uint32_t Default::getConfiguredOrDefaultMs(uint32_t configuredInterval, uint32_t defaultInterval)
{
    if (configuredInterval > 0)
        return configuredInterval * 1000;
    return defaultInterval * 1000;
}

uint32_t Default::getConfiguredOrDefaultMs(uint32_t configuredInterval)
{
    if (configuredInterval > 0)
        return configuredInterval * 1000;
    return default_broadcast_interval_secs * 1000;
}

uint32_t Default::getConfiguredOrDefault(uint32_t configured, uint32_t defaultValue)
{
    if (configured > 0)
        return configured;
    return defaultValue;
}
/**
 * Calculates the scaled value of the configured or default value in ms based on the number of online nodes.
 *
 * For example a default of 30 minutes (1800 seconds * 1000) would yield:
 *   45 nodes = 2475 * 1000
 *   60 nodes = 4500 * 1000
 *   75 nodes = 6525 * 1000
 *   90 nodes = 8550 * 1000
 * @param configured The configured value.
 * @param defaultValue The default value.
 * @param numOnlineNodes The number of online nodes.
 * @return The scaled value of the configured or default value.
 */
uint32_t Default::getConfiguredOrDefaultMsScaled(uint32_t configured, uint32_t defaultValue, uint32_t numOnlineNodes)
{
    // If we are a router, we don't scale the value. It's already significantly higher.
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER)
        return getConfiguredOrDefaultMs(configured, defaultValue);

    // Additionally if we're a tracker or sensor, we want priority to send position and telemetry
    if (IS_ONE_OF(config.device.role, meshtastic_Config_DeviceConfig_Role_SENSOR, meshtastic_Config_DeviceConfig_Role_TRACKER))
        return getConfiguredOrDefaultMs(configured, defaultValue);

    return getConfiguredOrDefaultMs(configured, defaultValue) * congestionScalingCoefficient(numOnlineNodes);
}

uint32_t Default::getConfiguredOrMinimumValue(uint32_t configured, uint32_t minValue)
{
    // If zero, intervals should be coalesced later by getConfiguredOrDefault... methods
    if (configured == 0)
        return configured;

    return configured < minValue ? minValue : configured;
}

uint8_t Default::getConfiguredOrDefaultHopLimit(uint8_t configured)
{
#if USERPREFS_EVENT_MODE
    return (configured > HOP_RELIABLE) ? HOP_RELIABLE : config.lora.hop_limit;
#else
    return (configured >= HOP_MAX) ? HOP_MAX : config.lora.hop_limit;
#endif
}

uint32_t Default::getLightlyScaledWindowMs(uint32_t baseSeconds, uint32_t numOnlineNodes, bool includeJitter)
{
    // Light congestion-aware scaling for intra-telemetry spacing windows.
    // Keeps behavior similar across roles but with smaller penalties than getConfiguredOrDefaultMsScaled.
    float coeff = Default::congestionScalingCoefficientLight((int)numOnlineNodes);
    uint32_t baseMs = baseSeconds * 1000;
    uint32_t scaled = (uint32_t)(baseMs * coeff);

    if (!includeJitter) return scaled;

    // Add mild +/-10% jitter to reduce synchronization across nodes.
    // random(min, max) is inclusive-exclusive on Arduino; guard if not available.
    uint32_t jitterRange = scaled / 20; // 10%
    uint32_t jitter = 0;
    #ifdef ARDUINO
    jitter = (uint32_t)random(0, (long)jitterRange);
    #else
    jitter = (uint32_t)(millis() % (jitterRange == 0 ? 1 : jitterRange));
    #endif
    bool subtract = ((millis() >> 2) & 0x1) != 0;
    if (subtract) {
        return (scaled > jitter) ? (scaled - jitter) : scaled;
    } else {
        return scaled + jitter;
    }
}
#include "MobilityOracle.h"
#include "GPSStatus.h"
#include "configuration.h" // fw+

// fw+ simple exponential smoothing of mobility factor based on GPS ground speed
static float s_mobilityFactor = 0.0f;
static uint32_t s_lastUpdateMs = 0;

// Map speed [m/s] to factor [0,1] with soft knee
static float speedToFactor(float metersPerSec)
{
    if (metersPerSec <= 0.2f) return 0.0f;
    if (metersPerSec >= 2.0f) return 1.0f;
    return (metersPerSec - 0.2f) / (2.0f - 0.2f);
}

float fwplus_getMobilityFactor01()
{
    uint32_t now = millis();
    // fw+ if static position is configured, treat as stationary and avoid extra computation
    if (config.position.fixed_position) {
        s_mobilityFactor = 0.0f;
        s_lastUpdateMs = now;
        return 0.0f;
    }
    float target = 0.0f;
    // Prefer GPS if available
    if (gpsStatus && gpsStatus->getHasLock()) {
        // ground_speed is in centimeters per second (per existing logs: *1e-2 for m/s)
        float speed_mps = ((float)gpsStatus->getSpeedCentimetersPerSecond()) * 1e-2f;
        // If GPSStatus has no getSpeed, approximate from Position p.ground_speed via NodeDB accessors.
        // For portability, clamp.
        if (speed_mps < 0.0f) speed_mps = 0.0f;
        if (speed_mps > 20.0f) speed_mps = 20.0f;
        target = speedToFactor(speed_mps);
    } else {
        // No GPS: conservative default (stationary). In future we can fold in radio volatility.
        target = 0.0f;
    }

    // Smooth with time-aware alpha to avoid jitter; more responsive when moving
    float alpha = 0.2f + 0.5f * target; // 0.2..0.7
    // fw+ if GPS is in power saving, be conservative and reduce responsiveness
    if (gpsStatus && gpsStatus->getIsPowerSaving()) {
        alpha *= 0.5f; // slow down adjustments
    }
    if (s_lastUpdateMs == 0) {
        s_mobilityFactor = target;
    } else {
        s_mobilityFactor = (1.0f - alpha) * s_mobilityFactor + alpha * target;
    }
    s_lastUpdateMs = now;
    if (s_mobilityFactor < 0.0f) s_mobilityFactor = 0.0f;
    if (s_mobilityFactor > 1.0f) s_mobilityFactor = 1.0f;
    return s_mobilityFactor;
}



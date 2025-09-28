#pragma once

// fw+ MobilityOracle — lightweight mobility estimator for automatic tuning
// Returns a 0..1 factor based primarily on GPS ground speed when available.
// Falls back to conservative 0 when speed is unknown.

#include <Arduino.h>

// fw+ compute current mobility as a factor in range [0, 1]
// 0 => stationary, 1 => highly mobile. Smoothed to avoid oscillations.
float fwplus_getMobilityFactor01();

// Convenience helper: consider node mobile when factor exceeds threshold
inline bool fwplus_isMobile() { return fwplus_getMobilityFactor01() > 0.5f; }



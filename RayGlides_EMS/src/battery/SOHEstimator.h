#ifndef SOH_ESTIMATOR_H
#define SOH_ESTIMATOR_H

#include <Arduino.h>

// Resets internal SOH tracking state (resistance estimate, cycle count).
void resetSOHEstimator();

// Estimates State of Health (0-100) from two independent signals:
//  1. Internal resistance, opportunistically estimated from consecutive
//     voltage/current samples whenever current changes enough to give a
//     usable dV/dI ratio, smoothed with an exponential moving average.
//     Rising resistance vs. a nominal "new battery" baseline indicates aging.
//  2. Equivalent full-cycle count, tracked via Coulomb throughput (total
//     amp-hours moved in/out, regardless of direction), with a simple
//     linear capacity-fade-per-cycle model.
// The lower (more pessimistic) of the two is returned, since health is
// limited by whichever degradation mechanism is furthest along.
// Call once per loop cycle with the latest voltage/current.
int estimateSOH(float voltage, float current);

// Informational accessors (useful for logging/telemetry, not required
// for the SOH value itself).
float getEstimatedInternalResistance();  // ohms
int getEquivalentCycleCount();

// Returns the full internal state, for persisting to EEPROM before a
// planned reset/shutdown.
void getSOHEstimatorState(float &resistanceOut, int &cycleCountOut, float &throughputOut);

// Restores previously-saved state after a reboot, so resistance drift
// and cycle count continue from where they left off instead of
// resetting to "brand new battery" assumptions.
void restoreSOHEstimatorState(float resistance, int cycleCount, float throughput);

#endif

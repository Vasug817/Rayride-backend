#include "SOHEstimator.h"
#include "config.h"

static float lastVoltage = 0;
static float lastCurrent = 0;
static float runningResistance = -1;   // Sentinel: no estimate yet
static float cumulativeThroughputAh = 0;
static int   cycleCount = 0;
static unsigned long lastUpdateMillis = 0;
static bool  initialized = false;

void resetSOHEstimator() {
  lastVoltage = 0;
  lastCurrent = 0;
  runningResistance = -1;
  cumulativeThroughputAh = 0;
  cycleCount = 0;
  initialized = false;
}

static void updateInternalResistanceEstimate(float voltage, float current) {
  float dV = voltage - lastVoltage;
  float dI = current - lastCurrent;

  // Only trust the sample if current changed enough to give a meaningful
  // ratio - a tiny dI makes dV/dI wildly noisy (dividing by near-zero).
  if (fabs(dI) >= SOH_MIN_DELTA_I_FOR_R_ESTIMATE) {
    float rSample = fabs(dV / dI);

    // Reject implausible outliers rather than letting one noisy sample
    // corrupt the running average.
    if (rSample <= SOH_MAX_PLAUSIBLE_RESISTANCE_OHMS) {
      if (runningResistance < 0) {
        runningResistance = rSample;   // First valid sample - take it directly
      } else {
        runningResistance = (SOH_R_EMA_ALPHA * runningResistance) +
                             ((1.0 - SOH_R_EMA_ALPHA) * rSample);
      }
    }
  }
}

int estimateSOH(float voltage, float current) {
  unsigned long now = millis();

  if (!initialized) {
    lastVoltage = voltage;
    lastCurrent = current;
    lastUpdateMillis = now;
    initialized = true;
    return 100;  // No history yet - assume full health until proven otherwise
  }

  float dtSeconds = (now - lastUpdateMillis) / 1000.0;
  lastUpdateMillis = now;

  updateInternalResistanceEstimate(voltage, current);

  // Coulomb throughput: count magnitude of current moved in EITHER
  // direction (charge or discharge both stress the cells and count
  // toward an equivalent full cycle).
  cumulativeThroughputAh += fabs(current) * (dtSeconds / 3600.0);
  while (cumulativeThroughputAh >= BATTERY_CAPACITY_AH) {
    cumulativeThroughputAh -= BATTERY_CAPACITY_AH;
    cycleCount++;
  }

  lastVoltage = voltage;
  lastCurrent = current;

  // --- Resistance-based health ---
  float sohResistance = 100.0;
  if (runningResistance > 0) {
    sohResistance = (BATTERY_NOMINAL_INTERNAL_RESISTANCE_OHMS / runningResistance) * 100.0;
    sohResistance = constrain(sohResistance, 0.0, 100.0);
  }

  // --- Cycle-count-based health (simple linear fade model) ---
  float sohCycles = 100.0 - (cycleCount * SOH_DEGRADATION_PER_CYCLE_PCT);
  sohCycles = constrain(sohCycles, 0.0, 100.0);

  // Health is limited by whichever indicator is worse
  float soh = min(sohResistance, sohCycles);
  return (int)round(soh);
}

float getEstimatedInternalResistance() {
  return runningResistance > 0 ? runningResistance : BATTERY_NOMINAL_INTERNAL_RESISTANCE_OHMS;
}

int getEquivalentCycleCount() {
  return cycleCount;
}

void getSOHEstimatorState(float &resistanceOut, int &cycleCountOut, float &throughputOut) {
  resistanceOut = runningResistance;
  cycleCountOut = cycleCount;
  throughputOut = cumulativeThroughputAh;
}

void restoreSOHEstimatorState(float resistance, int cycleCountIn, float throughput) {
  runningResistance = resistance;
  cycleCount = cycleCountIn;
  cumulativeThroughputAh = throughput;
  lastVoltage = 0;
  lastCurrent = 0;
  lastUpdateMillis = millis();
  initialized = true;   // Skip the "no history yet" branch on the next call
}

#include "SOCEstimator.h"
#include "config.h"

// OCV (open-circuit voltage) -> SOC lookup table for the pack, matching
// the operating range already used for fault thresholds (UNDER_VOLT_MIN
// to OVER_VOLT_MAX). Voltages must be strictly increasing.
struct OCVPoint { float voltage; float soc; };

static const OCVPoint OCV_TABLE[] = {
  { 42.0,  0.0 },
  { 46.0,  5.0 },
  { 48.0, 10.0 },
  { 50.0, 20.0 },
  { 52.0, 35.0 },
  { 54.0, 50.0 },
  { 56.0, 65.0 },
  { 58.0, 78.0 },
  { 60.0, 88.0 },
  { 62.0, 94.0 },
  { 64.0, 97.0 },
  { 66.0, 99.0 },
  { 68.0, 100.0 }
};
static const int OCV_TABLE_SIZE = sizeof(OCV_TABLE) / sizeof(OCVPoint);

static float coulombSOC = -1;          // Sentinel: not yet initialized
static unsigned long lastUpdateMillis = 0;
static bool initialized = false;

void resetSOCEstimator() {
  coulombSOC = -1;
  initialized = false;
  lastUpdateMillis = millis();
}

float getSOCEstimatorState() {
  return coulombSOC;
}

void restoreSOCEstimatorState(float savedSOC) {
  coulombSOC = constrain(savedSOC, 0.0, 100.0);
  initialized = true;   // Skip the "anchor to OCV" branch on the next call
  lastUpdateMillis = millis();
}

float estimateSOC_OCV(float voltage) {
  if (voltage <= OCV_TABLE[0].voltage) return OCV_TABLE[0].soc;
  if (voltage >= OCV_TABLE[OCV_TABLE_SIZE - 1].voltage) return OCV_TABLE[OCV_TABLE_SIZE - 1].soc;

  for (int i = 0; i < OCV_TABLE_SIZE - 1; i++) {
    float vLow = OCV_TABLE[i].voltage;
    float vHigh = OCV_TABLE[i + 1].voltage;
    if (voltage >= vLow && voltage <= vHigh) {
      float fraction = (voltage - vLow) / (vHigh - vLow);
      float socLow = OCV_TABLE[i].soc;
      float socHigh = OCV_TABLE[i + 1].soc;
      return socLow + fraction * (socHigh - socLow);
    }
  }
  return 50.0;  // Should be unreachable given the bounds checks above
}

float estimateSOC_CoulombCounting(float current, float dtSeconds) {
  if (coulombSOC < 0) coulombSOC = 50.0;  // Fallback starting point if used standalone

  float deltaAh = current * (dtSeconds / 3600.0);  // Amp-seconds -> amp-hours
  coulombSOC += (deltaAh / BATTERY_CAPACITY_AH) * 100.0;
  coulombSOC = constrain(coulombSOC, 0.0, 100.0);
  return coulombSOC;
}

float estimateSOC_Hybrid(float voltage, float current) {
  unsigned long now = millis();

  if (!initialized) {
    // First call: no history to integrate from, so anchor entirely to
    // the voltage-based estimate.
    coulombSOC = estimateSOC_OCV(voltage);
    lastUpdateMillis = now;
    initialized = true;
    return coulombSOC;
  }

  float dtSeconds = (now - lastUpdateMillis) / 1000.0;
  lastUpdateMillis = now;

  estimateSOC_CoulombCounting(current, dtSeconds);  // Updates coulombSOC in place

  if (fabs(current) <= SOC_REST_CURRENT_THRESHOLD_A) {
    // Close enough to at-rest that the voltage reading is trustworthy -
    // blend the Coulomb-counted value toward the OCV estimate to correct
    // any drift that's accumulated from sensor noise/offset.
    float ocvEstimate = estimateSOC_OCV(voltage);
    coulombSOC = (SOC_BLEND_ALPHA * coulombSOC) + ((1.0 - SOC_BLEND_ALPHA) * ocvEstimate);
  }

  coulombSOC = constrain(coulombSOC, 0.0, 100.0);
  return coulombSOC;
}

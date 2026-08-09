#ifndef SOC_ESTIMATOR_H
#define SOC_ESTIMATOR_H

#include <Arduino.h>

// Voltage-based SOC estimate using an Open-Circuit-Voltage (OCV) lookup
// table with piecewise-linear interpolation. Accurate when the battery
// is at rest (no significant current flowing) - under load, internal
// resistance causes voltage sag that makes this estimate pessimistic
// while discharging and optimistic while charging.
float estimateSOC_OCV(float voltage);

// Coulomb counting: integrates current over time (amp-hours in/out of
// the battery) to track SOC continuously, independent of voltage sag.
// Drifts over time due to sensor offset/noise accumulating, so it needs
// periodic correction from a voltage-based estimate.
float estimateSOC_CoulombCounting(float current, float dtSeconds);

// Hybrid estimate: Coulomb counting for continuous tracking, corrected
// (re-anchored) toward the OCV estimate whenever the battery is close
// enough to at rest for the voltage reading to be trustworthy. This is
// the method BatteryMonitor actually uses for BatteryData.soc.
float estimateSOC_Hybrid(float voltage, float current);

// Resets internal Coulomb-counting state. Call once at startup, or
// whenever you want to force a fresh re-anchor to the OCV estimate.
void resetSOCEstimator();

// Returns the current Coulomb-counted SOC value, for persisting to
// EEPROM before a planned reset/shutdown.
float getSOCEstimatorState();

// Restores a previously-saved SOC value after a reboot, skipping the
// normal "anchor to OCV on first call" behavior since we already have
// a trusted starting point.
void restoreSOCEstimatorState(float savedSOC);

#endif

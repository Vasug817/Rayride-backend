#ifndef SOLAR_MONITOR_H
#define SOLAR_MONITOR_H

#include <Arduino.h>

struct SolarData {
  int   rawVoltage;   // Raw ADC reading from the solar voltage divider pin
  int   rawCurrent;   // Raw ADC reading from the solar current sensor pin
  float voltage;      // Panel voltage, in volts
  float current;      // Panel current, in amps
  float power;         // Panel power output, in watts (voltage * current)
};

// Reads the solar voltage and current sensors and returns them together
// with derived power as one snapshot - the single point where raw ADC
// values become physically meaningful solar data. Every other module
// (ChargingDecision, fault detection, protocol reporting) works with
// SolarData, never the raw pins directly.
SolarData readSolarData();

#endif

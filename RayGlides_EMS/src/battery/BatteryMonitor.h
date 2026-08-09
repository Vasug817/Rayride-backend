#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>

struct BatteryData {
  int   rawVoltage;     // Raw ADC reading from the voltage divider pin
  float voltage;        // Actual battery voltage, in volts
  float current;        // Battery current, in amps (+ = charging)
  float temperature;    // Battery temperature, in degrees Celsius
  int   soc;             // State of charge, 0-100 (derived from voltage)
  int   soh;             // State of health, 0-100 (derived from resistance + cycling)
};

// Reads all three battery sensors (voltage divider, current sensor,
// temperature sensor) and returns them together as one snapshot. This is
// the single point where raw ADC values become physically meaningful
// battery data - every other module works with BatteryData, never raw pins.
BatteryData readBatteryData();

#endif

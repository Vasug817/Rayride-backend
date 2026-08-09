#ifndef BATTERY_STATE_MACHINE_H
#define BATTERY_STATE_MACHINE_H

#include <Arduino.h>

enum ChargeState { 
  STATE_BOOT, 
  STATE_SELF_TEST, 
  STATE_IDLE, 
  STATE_SOLAR_AVAILABLE, 
  STATE_CHARGING, 
  STATE_FULLY_CHARGED, 
  STATE_FAULT 
};

const char* stateName(ChargeState s);

ChargeState evaluateBatteryState(ChargeState current, float batteryVoltage, float solarVoltage, int soc, bool criticalFault, unsigned long timeMs);

#endif

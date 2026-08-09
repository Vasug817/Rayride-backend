#ifndef THERMAL_MANAGER_H
#define THERMAL_MANAGER_H

#include <Arduino.h>

#define FAN_PWM_PIN       18
#define FAN_PWM_CHANNEL    1
#define FAN_PWM_FREQ    5000
#define FAN_PWM_RES        8

// Initialize thermal managers and GPIO PWM settings
void initThermalManager();

// Update fan speeds and protection derating based on current heatsink temperature
void updateThermalManager(float mpptTemp, unsigned long timeMs);

// Get current Fan Speed duty cycle (fraction 0.0 to 1.0)
float getFanDuty();

// Retrieve thermal power derating factor (multiplier 0.0 to 1.0)
float getMPPTPowerDerateFactor();

// Check if system is locked in thermal emergency shutdown
bool isThermalShutdownActive();

// Override fan speed manually (0.0 to 1.0)
void setFanManualOverride(bool enable, float duty);

#endif // THERMAL_MANAGER_H

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

// Parameter IDs for dynamic configuration updates
#define PARAM_BATTERY_OVER_VOLTAGE  1
#define PARAM_BATTERY_UNDER_VOLTAGE 2
#define PARAM_BATTERY_OVER_CURRENT  3
#define PARAM_BATTERY_OVER_TEMP     4
#define PARAM_SOLAR_VOLTAGE_LIMIT   5
#define PARAM_FAN_TEMP_ON           6
#define PARAM_FAN_TEMP_OFF          7
#define PARAM_CHARGING_RESUME_SOC   8

struct SystemConfig {
  float batteryOverVoltage;
  float batteryUnderVoltage;
  float batteryOverCurrent;
  float batteryOverTemp;
  float solarVoltageLimit;
  float fanTempOnThreshold;
  float fanTempOffThreshold;
  float chargingResumeSOC;
};

// Global config access
extern SystemConfig sysConfig;

// Initialize Configuration (loads from NVS Preferences, or writes default fallback)
void initConfig();

// Retrieve configuration copies
SystemConfig getSystemConfig();

// Save new configuration struct to NVS
void setSystemConfig(SystemConfig cfg);

// Update a single parameter dynamically and save to NVS
bool updateConfigParameter(uint8_t paramId, float value);

// Print current configuration values to console
void printConfig();

#endif // CONFIG_MANAGER_H

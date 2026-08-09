#ifndef BATTERY_ANALYTICS_H
#define BATTERY_ANALYTICS_H

#include <Arduino.h>

struct AnalyticsStats {
  float maxVoltage;
  float minVoltage;
  float maxCurrent; // peak charge current
  float minCurrent; // peak discharge current (negative)
  float maxTemp;
  float minTemp;
  uint32_t cycleCount;
  float accumulatedDischargeAh;
};

extern AnalyticsStats batteryStats;

// Initialize analytics state (restores SOC, SOH, cycles from NVS)
void initBatteryAnalytics();

// Update battery analytics estimators
void updateBatteryAnalytics(float battV, float battI, float temp, unsigned long timeMs);

// Get current State of Charge (%)
int getAnalyticsSOC();

// Get current State of Health (%)
int getAnalyticsSOH();

// Force an SOC reset value
void forceSOC(int value);

// Print analytics summary
void printAnalyticsStats();

#endif // BATTERY_ANALYTICS_H

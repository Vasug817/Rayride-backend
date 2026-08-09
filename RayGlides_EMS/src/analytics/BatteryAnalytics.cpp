#include "analytics/BatteryAnalytics.h"
#include <Preferences.h>
#include "debug/DebugLog.h"
#include "energy/EnergyEngine.h"
#include <math.h>

#define CAPACITY_AH 20.0f
#define ALPHA_BLEND 0.05f

AnalyticsStats batteryStats;
static int currentSOC = 100;
static int currentSOH = 100;
static unsigned long lastUpdateMs = 0;
static Preferences analyticsPrefs;
static unsigned long lastSaveTime = 0;

static int estimateOCVSOC(float voltage) {
  if (voltage >= 54.0f) return 100;
  if (voltage >= 52.0f) return 80 + (int)((voltage - 52.0f) * 10.0f);
  if (voltage >= 50.0f) return 60 + (int)((voltage - 50.0f) * 10.0f);
  if (voltage >= 48.0f) return 40 + (int)((voltage - 48.0f) * 10.0f);
  if (voltage >= 45.0f) return 20 + (int)((voltage - 45.0f) * 6.67f);
  if (voltage >= 42.0f) return (int)((voltage - 42.0f) * 6.67f);
  return 0;
}

void initBatteryAnalytics() {
  analyticsPrefs.begin("analytics", false);
  
  currentSOC = analyticsPrefs.getInt("soc", 100);
  currentSOH = analyticsPrefs.getInt("soh", 100);
  batteryStats.cycleCount = analyticsPrefs.getUInt("cycles", 0);
  batteryStats.accumulatedDischargeAh = analyticsPrefs.getFloat("dis_ah", 0.0f);
  
  batteryStats.maxVoltage = analyticsPrefs.getFloat("max_v", 52.0f);
  batteryStats.minVoltage = analyticsPrefs.getFloat("min_v", 52.0f);
  batteryStats.maxCurrent = analyticsPrefs.getFloat("max_i", 0.0f);
  batteryStats.minCurrent = analyticsPrefs.getFloat("min_i", 0.0f);
  batteryStats.maxTemp = analyticsPrefs.getFloat("max_t", 25.0f);
  batteryStats.minTemp = analyticsPrefs.getFloat("min_t", 25.0f);
  
  analyticsPrefs.end();

  lastUpdateMs = millis();
  lastSaveTime = millis();
  logInfo("ANALYTICS", "Battery Analytics initialized.");
}

void updateBatteryAnalytics(float battV, float battI, float temp, unsigned long timeMs) {
  if (isnan(battV) || isnan(battI) || isnan(temp)) return;

  // Track Peak Stats
  if (battV > batteryStats.maxVoltage) batteryStats.maxVoltage = battV;
  if (battV < batteryStats.minVoltage) batteryStats.minVoltage = battV;
  if (battI > batteryStats.maxCurrent) batteryStats.maxCurrent = battI;
  if (battI < batteryStats.minCurrent) batteryStats.minCurrent = battI;
  if (temp > batteryStats.maxTemp) batteryStats.maxTemp = temp;
  if (temp < batteryStats.minTemp) batteryStats.minTemp = temp;

  float dtHours = (float)(timeMs - lastUpdateMs) / 3600000.0f;
  if (dtHours <= 0.0f) return;

  // 1. Coulomb Counting
  float deltaSOC = (battI * dtHours / CAPACITY_AH) * 100.0f;
  float newSOC = (float)currentSOC + deltaSOC;

  // 2. OCV Blending
  if (fabs(battI) < 0.5f) {
    int ocvSOC = estimateOCVSOC(battV);
    newSOC = (1.0f - ALPHA_BLEND) * newSOC + ALPHA_BLEND * (float)ocvSOC;
  }

  currentSOC = constrain((int)round(newSOC), 0, 100);

  // 3. Cycles & Health
  if (battI < 0.0f) {
    float dischargeAh = -battI * dtHours;
    batteryStats.accumulatedDischargeAh += dischargeAh;
    
    uint32_t currentCycles = (uint32_t)(batteryStats.accumulatedDischargeAh / CAPACITY_AH);
    if (currentCycles > batteryStats.cycleCount) {
      uint32_t cyclesDelta = currentCycles - batteryStats.cycleCount;
      batteryStats.cycleCount = currentCycles;
      currentSOH = constrain(currentSOH - (int)(cyclesDelta * 1), 50, 100);
      
      char logMsg[64];
      snprintf(logMsg, sizeof(logMsg), "Equivalent cycle accumulated! Total Cycles: %u, SOH: %d%%", 
        batteryStats.cycleCount, currentSOH);
      logInfo("ANALYTICS", logMsg);
    }
  }

  lastUpdateMs = timeMs;

  if (timeMs - lastSaveTime > 15000) {
    analyticsPrefs.begin("analytics", false);
    analyticsPrefs.putInt("soc", currentSOC);
    analyticsPrefs.putInt("soh", currentSOH);
    analyticsPrefs.putUInt("cycles", batteryStats.cycleCount);
    analyticsPrefs.putFloat("dis_ah", batteryStats.accumulatedDischargeAh);
    analyticsPrefs.putFloat("max_v", batteryStats.maxVoltage);
    analyticsPrefs.putFloat("min_v", batteryStats.minVoltage);
    analyticsPrefs.putFloat("max_i", batteryStats.maxCurrent);
    analyticsPrefs.putFloat("min_i", batteryStats.minCurrent);
    analyticsPrefs.putFloat("max_t", batteryStats.maxTemp);
    analyticsPrefs.putFloat("min_t", batteryStats.minTemp);
    analyticsPrefs.end();
    lastSaveTime = timeMs;
  }
}

int getAnalyticsSOC() { return currentSOC; }
int getAnalyticsSOH() { return currentSOH; }
void forceSOC(int value) { currentSOC = constrain(value, 0, 100); }

void printAnalyticsStats() {
  char buf[256];
  snprintf(buf, sizeof(buf), 
    "ANALYTICS: SOC=%d%% SOH=%d%% Cycles=%u PeakV=%.1f-%.1f PeakI=%.1f-%.1f PeakT=%.1f-%.1f",
    currentSOC, currentSOH, batteryStats.cycleCount,
    batteryStats.minVoltage, batteryStats.maxVoltage,
    batteryStats.minCurrent, batteryStats.maxCurrent,
    batteryStats.minTemp, batteryStats.maxTemp);
  logInfo("ANALYTICS", buf);
}

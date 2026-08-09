#include "thermal/ThermalManager.h"
#include "configuration/ConfigManager.h"
#include "fault/FaultManager.h"
#include "debug/DebugLog.h"

static float currentFanDuty = 0.0f;
static float powerDerateFactor = 1.0f;
static bool thermalShutdownActive = false;
static bool fanActive = false;

static bool manualFanOverride = false;
static float manualFanDuty = 0.0f;

void initThermalManager() {
  currentFanDuty = 0.0f;
  powerDerateFactor = 1.0f;
  thermalShutdownActive = false;
  fanActive = false;
  manualFanOverride = false;
  manualFanDuty = 0.0f;

  ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RES);
  ledcAttachPin(FAN_PWM_PIN, FAN_PWM_CHANNEL);
  ledcWrite(FAN_PWM_CHANNEL, 0); 
  logInfo("THERMAL", "Thermal Manager initialized (Fan on GPIO 18).");
}

void setFanManualOverride(bool enable, float duty) {
  manualFanOverride = enable;
  if (duty < 0.0f) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;
  manualFanDuty = duty;
  char msg[64];
  snprintf(msg, sizeof(msg), "Fan manual override: %s (duty=%d%%)", enable ? "ENABLED" : "DISABLED", (int)(duty * 100));
  logInfo("THERMAL", msg);
}

void updateThermalManager(float temp, unsigned long timeMs) {
  SystemConfig cfg = getSystemConfig();

  // 1. Fan Control Logic (Hysteresis and dynamic scaling / manual override)
  if (manualFanOverride) {
    currentFanDuty = manualFanDuty;
  } else {
    if (temp >= cfg.fanTempOnThreshold) {
      fanActive = true;
    } else if (temp < cfg.fanTempOffThreshold) {
      fanActive = false;
    }

    if (fanActive) {
      float range = 55.0f - cfg.fanTempOnThreshold;
      if (range <= 0.0f) range = 1.0f;
      float ratio = (temp - cfg.fanTempOnThreshold) / range;
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 1.0f) ratio = 1.0f;
      currentFanDuty = 0.3f + (ratio * 0.7f); // Scales 30% to 100%
    } else {
      currentFanDuty = 0.0f;
    }
  }

  // Update hardware PWM
  uint32_t dutyVal = (uint32_t)(currentFanDuty * 255);
  ledcWrite(FAN_PWM_CHANNEL, dutyVal);

  // 2. Warnings and Derating
  if (temp > 50.0f) {
    static unsigned long lastWarningLog = 0;
    if (timeMs - lastWarningLog > 5000) {
      logWarn("THERMAL", "High temperature warning! Fan running at max speed.");
      lastWarningLog = timeMs;
    }
  }

  if (temp > 55.0f) {
    powerDerateFactor = 1.0f - (temp - 55.0f) / 10.0f;
    if (powerDerateFactor < 0.0f) powerDerateFactor = 0.0f;
    
    static unsigned long lastDerateLog = 0;
    if (timeMs - lastDerateLog > 10000) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Overheating! Derating charger power to %d%%", (int)(powerDerateFactor * 100));
      logWarn("THERMAL", msg);
      lastDerateLog = timeMs;
    }
  } else {
    powerDerateFactor = 1.0f;
  }

  // 3. Emergency Shutdown
  if (temp >= 65.0f) {
    if (!thermalShutdownActive) {
      thermalShutdownActive = true;
      logError("THERMAL", "Critical over-temperature! Emergency shutdown initiated!");
      triggerFault(F012_THERMAL_SHUTDOWN, SEV_CRITICAL);
    }
  } else if (temp < 50.0f) { 
    if (thermalShutdownActive) {
      thermalShutdownActive = false;
      logInfo("THERMAL", "Enclosure temperature returned to safe zone. Thermal shutdown cleared.");
      clearFault(F012_THERMAL_SHUTDOWN);
    }
  }
}

float getFanDuty() {
  return currentFanDuty;
}

float getMPPTPowerDerateFactor() {
  return powerDerateFactor;
}

bool isThermalShutdownActive() {
  return thermalShutdownActive;
}

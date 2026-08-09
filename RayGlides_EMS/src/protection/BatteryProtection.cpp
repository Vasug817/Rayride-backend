#include "protection/BatteryProtection.h"
#include "configuration/ConfigManager.h"
#include "debug/DebugLog.h"
#include <math.h>

ProtectionStatus currentProtection;

static unsigned long overVoltageClearTime = 0;
static unsigned long underVoltageClearTime = 0;
static unsigned long overCurrentClearTime = 0;
static unsigned long overTempClearTime = 0;

void initBatteryProtection() {
  currentProtection.overVoltageFault = false;
  currentProtection.underVoltageFault = false;
  currentProtection.overCurrentFault = false;
  currentProtection.overTempFault = false;
  currentProtection.chargeDisable = false;
  currentProtection.loadDisable = false;
}

ProtectionStatus checkBatteryProtection(SensorReading readings, unsigned long timeMs) {
  if (isnan(readings.batteryVoltage)) return currentProtection;

  SystemConfig cfg = getSystemConfig();

  // 1. Over-Voltage Check
  if (readings.batteryVoltage > cfg.batteryOverVoltage) {
    if (!currentProtection.overVoltageFault) {
      currentProtection.overVoltageFault = true;
      logError("BPS", "Battery Over-Voltage Protection Triggered!");
    }
    overVoltageClearTime = 0;
  } else if (readings.batteryVoltage < (cfg.batteryOverVoltage - 1.5f)) {
    if (currentProtection.overVoltageFault) {
      if (overVoltageClearTime == 0) {
        overVoltageClearTime = timeMs;
      } else if (timeMs - overVoltageClearTime > 3000) {
        currentProtection.overVoltageFault = false;
        logInfo("BPS", "Battery Over-Voltage Protection Recovered.");
        overVoltageClearTime = 0;
      }
    }
  } else {
    overVoltageClearTime = 0;
  }

  // 2. Under-Voltage Check
  if (readings.batteryVoltage < cfg.batteryUnderVoltage) {
    if (!currentProtection.underVoltageFault) {
      currentProtection.underVoltageFault = true;
      logError("BPS", "Battery Under-Voltage Protection Triggered!");
    }
    underVoltageClearTime = 0;
  } else if (readings.batteryVoltage > (cfg.batteryUnderVoltage + 1.5f)) {
    if (currentProtection.underVoltageFault) {
      if (underVoltageClearTime == 0) {
        underVoltageClearTime = timeMs;
      } else if (timeMs - underVoltageClearTime > 3000) {
        currentProtection.underVoltageFault = false;
        logInfo("BPS", "Battery Under-Voltage Protection Recovered.");
        underVoltageClearTime = 0;
      }
    }
  } else {
    underVoltageClearTime = 0;
  }

  // 3. Over-Current Check
  float absCurrent = fabs(readings.batteryCurrent);
  if (absCurrent > cfg.batteryOverCurrent) {
    if (!currentProtection.overCurrentFault) {
      currentProtection.overCurrentFault = true;
      logError("BPS", "Battery Over-Current Protection Triggered!");
    }
    overCurrentClearTime = 0;
  } else if (absCurrent < (cfg.batteryOverCurrent - 2.0f)) {
    if (currentProtection.overCurrentFault) {
      if (overCurrentClearTime == 0) {
        overCurrentClearTime = timeMs;
      } else if (timeMs - overCurrentClearTime > 3000) {
        currentProtection.overCurrentFault = false;
        logInfo("BPS", "Battery Over-Current Protection Recovered.");
        overCurrentClearTime = 0;
      }
    }
  } else {
    overCurrentClearTime = 0;
  }

  // 4. Over-Temperature Check
  if (readings.batteryTemp > cfg.batteryOverTemp) {
    if (!currentProtection.overTempFault) {
      currentProtection.overTempFault = true;
      logError("BPS", "Battery Over-Temperature Protection Triggered!");
    }
    overTempClearTime = 0;
  } else if (readings.batteryTemp < (cfg.batteryOverTemp - 3.0f)) {
    if (currentProtection.overTempFault) {
      if (overTempClearTime == 0) {
        overTempClearTime = timeMs;
      } else if (timeMs - overTempClearTime > 3000) {
        currentProtection.overTempFault = false;
        logInfo("BPS", "Battery Over-Temperature Protection Recovered.");
        overTempClearTime = 0;
      }
    }
  } else {
    overTempClearTime = 0;
  }

  // 5. Charging / Load Disabling Logic
  currentProtection.chargeDisable = currentProtection.overVoltageFault || currentProtection.overTempFault;
  currentProtection.loadDisable = currentProtection.underVoltageFault || currentProtection.overTempFault || currentProtection.overCurrentFault;

  return currentProtection;
}

bool isChargeAllowed() {
  return !currentProtection.chargeDisable;
}

bool isLoadAllowed() {
  return !currentProtection.loadDisable;
}

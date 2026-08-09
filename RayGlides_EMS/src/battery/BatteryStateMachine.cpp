#include "battery/BatteryStateMachine.h"
#include "configuration/ConfigManager.h"
#include "debug/DebugLog.h"

static unsigned long stateTimer = 0;

const char* stateName(ChargeState s) {
  switch (s) {
    case STATE_BOOT:            return "BOOT";
    case STATE_SELF_TEST:       return "SELF_TEST";
    case STATE_IDLE:            return "IDLE";
    case STATE_SOLAR_AVAILABLE: return "SOLAR_AVAILABLE";
    case STATE_CHARGING:        return "CHARGING";
    case STATE_FULLY_CHARGED:   return "FULLY_CHARGED";
    case STATE_FAULT:           return "FAULT";
  }
  return "UNKNOWN";
}

ChargeState evaluateBatteryState(ChargeState current, float batteryVoltage, float solarVoltage, int soc, bool criticalFault, unsigned long timeMs) {
  if (criticalFault) {
    if (current != STATE_FAULT) {
      logError("STATE", "Unsafe condition detected! Entering FAULT state.");
    }
    return STATE_FAULT;
  }

  SystemConfig cfg = getSystemConfig();

  switch (current) {
    case STATE_BOOT:
      if (stateTimer == 0) {
        stateTimer = timeMs;
      } else if (timeMs - stateTimer > 1500) { // 1.5s boot delay
        stateTimer = 0;
        logInfo("STATE", "Boot delay finished. Transitioning to SELF_TEST.");
        return STATE_SELF_TEST;
      }
      return STATE_BOOT;

    case STATE_SELF_TEST:
      if (stateTimer == 0) {
        stateTimer = timeMs;
      } else if (timeMs - stateTimer > 1500) { // 1.5s self-test duration
        stateTimer = 0;
        logInfo("STATE", "Self-test passed. Entering IDLE.");
        return STATE_IDLE;
      }
      return STATE_SELF_TEST;

    case STATE_IDLE:
      if (soc >= 100) {
        return STATE_FULLY_CHARGED;
      }
      if (solarVoltage > 12.0f && solarVoltage < cfg.solarVoltageLimit) {
        logInfo("STATE", "Solar energy detected. Transitioning to SOLAR_AVAILABLE.");
        return STATE_SOLAR_AVAILABLE;
      }
      return STATE_IDLE;

    case STATE_SOLAR_AVAILABLE:
      if (soc >= 100) {
        return STATE_FULLY_CHARGED;
      }
      if (solarVoltage < 5.0f) {
        logInfo("STATE", "Solar voltage lost. Returning to IDLE.");
        return STATE_IDLE;
      }
      if (soc < cfg.chargingResumeSOC) {
        logInfo("STATE", "SOC below threshold. Starting CHARGING.");
        return STATE_CHARGING;
      }
      return STATE_SOLAR_AVAILABLE;

    case STATE_CHARGING:
      if (soc >= 100) {
        logInfo("STATE", "Battery fully charged. Entering FULLY_CHARGED.");
        return STATE_FULLY_CHARGED;
      }
      if (solarVoltage < 5.0f) {
        logInfo("STATE", "Solar voltage lost. Returning to IDLE.");
        return STATE_IDLE;
      }
      return STATE_CHARGING;

    case STATE_FULLY_CHARGED:
      if (soc < cfg.chargingResumeSOC) {
        logInfo("STATE", "Battery depleted below resume SOC. Re-enabling charging.");
        return STATE_CHARGING;
      }
      return STATE_FULLY_CHARGED;

    case STATE_FAULT:
      if (!criticalFault) {
        logInfo("STATE", "Critical fault cleared. Restarting via BOOT.");
        stateTimer = 0;
        return STATE_BOOT;
      }
      return STATE_FAULT;

    default:
      return current;
  }
}

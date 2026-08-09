#include "sensors/SensorSimulator.h"
#include "debug/DebugLog.h"
#include <math.h>

static uint8_t activeScenario = SCENARIO_NORMAL_OPERATION;
static unsigned long scenarioStartTime = 0;

void initSensorSimulator() {
  activeScenario = SCENARIO_NORMAL_OPERATION;
  scenarioStartTime = millis();
}

void setSimulationScenario(uint8_t scenarioId) {
  if (scenarioId <= SCENARIO_SENSOR_FAILURE) {
    activeScenario = scenarioId;
    scenarioStartTime = millis();
    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "Scenario Switched: %s", getScenarioName(scenarioId));
    logInfo("SIM", logMsg);
  }
}

uint8_t getSimulationScenario() {
  return activeScenario;
}

const char* getScenarioName(uint8_t scenarioId) {
  switch (scenarioId) {
    case SCENARIO_NORMAL_OPERATION: return "NORMAL_OPERATION";
    case SCENARIO_CHARGING:          return "CHARGING_RAMP";
    case SCENARIO_LOW_BATTERY:       return "LOW_BATTERY";
    case SCENARIO_FULL_BATTERY:      return "FULL_BATTERY";
    case SCENARIO_HIGH_TEMPERATURE:  return "HIGH_TEMPERATURE";
    case SCENARIO_HIGH_CURRENT:      return "HIGH_CURRENT";
    case SCENARIO_NO_SOLAR:          return "NO_SOLAR";
    case SCENARIO_SENSOR_FAILURE:    return "SENSOR_FAILURE";
    default:                         return "UNKNOWN";
  }
}

SensorReading getSimulatedReading(unsigned long timeMs) {
  SensorReading r;
  r.batteryVoltageHealthy = true;
  r.batteryCurrentHealthy = true;
  r.batteryTempHealthy = true;
  r.solarVoltageHealthy = true;
  r.solarCurrentHealthy = true;
  r.mpptTempHealthy = true;
  r.overallHealthy = true;

  unsigned long elapsedSec = (timeMs - scenarioStartTime) / 1000;

  switch (activeScenario) {
    case SCENARIO_NORMAL_OPERATION:
      r.batteryVoltage = 52.0f;
      r.batteryCurrent = 2.5f;
      r.batteryTemp = 25.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 8.0f;
      r.mpptTemp = 28.0f;
      break;

    case SCENARIO_CHARGING:
      r.batteryVoltage = 48.0f + (elapsedSec * 0.1f);
      if (r.batteryVoltage > 58.0f) r.batteryVoltage = 58.0f;
      r.batteryCurrent = 5.0f;
      r.batteryTemp = 25.0f + (elapsedSec * 0.1f);
      if (r.batteryTemp > 35.0f) r.batteryTemp = 35.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 8.0f;
      r.mpptTemp = 28.0f + (elapsedSec * 0.15f);
      if (r.mpptTemp > 43.0f) r.mpptTemp = 43.0f;
      break;

    case SCENARIO_LOW_BATTERY:
      r.batteryVoltage = 41.0f;
      r.batteryCurrent = 0.0f;
      r.batteryTemp = 24.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 0.0f;
      r.mpptTemp = 25.0f;
      break;

    case SCENARIO_FULL_BATTERY:
      r.batteryVoltage = 69.0f;
      r.batteryCurrent = 0.1f;
      r.batteryTemp = 26.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 0.2f;
      r.mpptTemp = 27.0f;
      break;

    case SCENARIO_HIGH_TEMPERATURE:
      r.batteryVoltage = 52.0f;
      r.batteryCurrent = 2.5f;
      r.batteryTemp = 25.0f + (elapsedSec * 12.0f);
      if (r.batteryTemp > 65.0f) r.batteryTemp = 65.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 8.0f;
      r.mpptTemp = 28.0f + (elapsedSec * 12.0f);
      if (r.mpptTemp > 68.0f) r.mpptTemp = 68.0f;
      break;

    case SCENARIO_HIGH_CURRENT:
      r.batteryVoltage = 50.0f;
      r.batteryCurrent = 2.5f + (elapsedSec * 10.0f);
      if (r.batteryCurrent > 30.0f) r.batteryCurrent = 30.0f;
      r.batteryTemp = 28.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 8.0f;
      r.mpptTemp = 30.0f;
      break;

    case SCENARIO_NO_SOLAR:
      r.batteryVoltage = 50.0f;
      r.batteryCurrent = -1.5f;
      r.batteryTemp = 25.0f;
      r.solarVoltage = 0.0f;
      r.solarCurrent = 0.0f;
      r.mpptTemp = 24.0f;
      break;

    case SCENARIO_SENSOR_FAILURE:
      r.batteryVoltage = NAN;
      r.batteryVoltageHealthy = false;
      r.overallHealthy = false;
      r.batteryCurrent = 0.0f;
      r.batteryTemp = 25.0f;
      r.solarVoltage = 24.0f;
      r.solarCurrent = 0.0f;
      r.mpptTemp = 25.0f;
      break;

    default:
      r.batteryVoltage = 52.0f;
      r.batteryCurrent = 0.0f;
      r.batteryTemp = 25.0f;
      r.solarVoltage = 0.0f;
      r.solarCurrent = 0.0f;
      r.mpptTemp = 25.0f;
      break;
  }
  return r;
}

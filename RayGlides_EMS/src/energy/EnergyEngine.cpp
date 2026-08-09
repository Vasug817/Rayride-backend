#include "energy/EnergyEngine.h"
#include "debug/DebugLog.h"

static float accumulatedSolarWh = 0.0f;
static float accumulatedChargingWh = 0.0f;
static float accumulatedConsumedWh = 0.0f;
static float accumulatedAh = 0.0f;

static float prevSolarPower = 0.0f;
static float prevChargingPower = 0.0f;
static float prevConsumedPower = 0.0f;
static float prevBatteryCurrent = 0.0f;
static unsigned long lastUpdateMs = 0;

void initEnergyEngine() {
  resetEnergyStats();
}

void updateEnergyEngine(float battV, float battI, float solarV, float solarI, unsigned long timeMs) {
  if (lastUpdateMs == 0) {
    lastUpdateMs = timeMs;
    prevSolarPower = solarV * solarI;
    
    float pBatt = battV * battI;
    if (pBatt > 0.0f) {
      prevChargingPower = pBatt;
      prevConsumedPower = 0.0f;
    } else {
      prevChargingPower = 0.0f;
      prevConsumedPower = -pBatt;
    }
    prevBatteryCurrent = battI;
    return;
  }

  float dtHours = (float)(timeMs - lastUpdateMs) / 3600000.0f;
  if (dtHours <= 0.0f) return;

  float currSolarPower = solarV * solarI;
  float currBattPower = battV * battI;
  float currChargingPower = 0.0f;
  float currConsumedPower = 0.0f;

  if (currBattPower > 0.0f) {
    currChargingPower = currBattPower;
  } else {
    currConsumedPower = -currBattPower;
  }

  // Integrate via trapezoidal rules
  accumulatedSolarWh += ((prevSolarPower + currSolarPower) / 2.0f) * dtHours;
  accumulatedChargingWh += ((prevChargingPower + currChargingPower) / 2.0f) * dtHours;
  accumulatedConsumedWh += ((prevConsumedPower + currConsumedPower) / 2.0f) * dtHours;
  accumulatedAh += ((prevBatteryCurrent + battI) / 2.0f) * dtHours;

  prevSolarPower = currSolarPower;
  prevChargingPower = currChargingPower;
  prevConsumedPower = currConsumedPower;
  prevBatteryCurrent = battI;
  lastUpdateMs = timeMs;
}

float getAccumulatedSolarWh() { return accumulatedSolarWh; }
float getAccumulatedChargingWh() { return accumulatedChargingWh; }
float getAccumulatedConsumedWh() { return accumulatedConsumedWh; }
float getAccumulatedAh() { return accumulatedAh; }

void resetEnergyStats() {
  accumulatedSolarWh = 0.0f;
  accumulatedChargingWh = 0.0f;
  accumulatedConsumedWh = 0.0f;
  accumulatedAh = 0.0f;
  prevSolarPower = 0.0f;
  prevChargingPower = 0.0f;
  prevConsumedPower = 0.0f;
  prevBatteryCurrent = 0.0f;
  lastUpdateMs = 0;
  logInfo("ENERGY", "Energy accumulators reset.");
}

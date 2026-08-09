#include "DataLogger.h"
#include "config.h"
#include "../storage/EEPROMStorage.h"
#include "../battery/SOCEstimator.h"
#include "../battery/SOHEstimator.h"
#include "../debug/DebugLog.h"

static LogRecord ramBuffer[DATALOGGER_MAX_RECORDS];
static int ramHead = 0;
static int ramCount = 0;

static unsigned long lastRecordMillis = 0;
static unsigned long lastCheckpointMillis = 0;

void initDataLogger() {
  ramHead = 0;
  ramCount = 0;
  lastRecordMillis = 0;
  lastCheckpointMillis = millis();

  initEEPROMStorage();

  PersistedEstimatorState saved;
  if (loadPersistedState(saved)) {
    restoreSOCEstimatorState(saved.coulombSOC);
    restoreSOHEstimatorState(saved.sohResistance, saved.sohCycleCount, saved.sohThroughputAh);

    char msg[80];
    snprintf(msg, sizeof(msg), "Restored from EEPROM: SOC=%.1f%% R=%.3fohm cycles=%d",
             saved.coulombSOC, saved.sohResistance, saved.sohCycleCount);
    logInfo("DATALOGGER", msg);
  } else {
    logInfo("DATALOGGER", "No valid EEPROM checkpoint found - starting fresh");
  }
}

static void recordSnapshot(BatteryData battery, SolarData solar, ChargeState state,
                            ChargingMode mode, FaultCode fault) {
  LogRecord r;
  r.timestamp = millis();
  r.battVoltage = battery.voltage;
  r.battCurrent = battery.current;
  r.battTemp = battery.temperature;
  r.battSOC = battery.soc;
  r.battSOH = battery.soh;
  r.solarVoltage = solar.voltage;
  r.solarPower = solar.power;
  r.state = (uint8_t)state;
  r.mode = (uint8_t)mode;
  r.fault = (uint8_t)fault;

  ramBuffer[ramHead] = r;
  ramHead = (ramHead + 1) % DATALOGGER_MAX_RECORDS;
  if (ramCount < DATALOGGER_MAX_RECORDS) ramCount++;
}

static void checkpointToEEPROM() {
  PersistedEstimatorState state;
  state.coulombSOC = getSOCEstimatorState();
  getSOHEstimatorState(state.sohResistance, state.sohCycleCount, state.sohThroughputAh);

  savePersistedState(state);
  logInfo("DATALOGGER", "Estimator state checkpointed to EEPROM");
}

void updateDataLogger(BatteryData battery, SolarData solar, ChargeState state,
                       ChargingMode mode, FaultCode fault) {
  unsigned long now = millis();

  if (now - lastRecordMillis >= DATALOGGER_RECORD_INTERVAL_MS) {
    recordSnapshot(battery, solar, state, mode, fault);
    lastRecordMillis = now;
  }

  if (now - lastCheckpointMillis >= DATALOGGER_CHECKPOINT_INTERVAL_MS) {
    checkpointToEEPROM();
    lastCheckpointMillis = now;
  }
}

int getLogRecordCount() {
  return ramCount;
}

LogRecord getLogRecord(int indexFromNewest) {
  int idx = (ramHead - 1 - indexFromNewest + DATALOGGER_MAX_RECORDS * 2) % DATALOGGER_MAX_RECORDS;
  return ramBuffer[idx];
}

void clearDataLog() {
  ramHead = 0;
  ramCount = 0;
}

void printDataLog() {
  char header[40];
  snprintf(header, sizeof(header), "--- Data Log (%d records) ---", ramCount);
  logInfo("DATALOGGER", header);

  for (int i = 0; i < ramCount; i++) {
    LogRecord r = getLogRecord(i);
    char line[128];
    snprintf(line, sizeof(line),
      "[%lums] V=%.1f I=%.1f T=%.1f SOC=%d%% SOH=%d%% | SolarV=%.1f P=%.1fW | state=%d mode=%d fault=%d",
      r.timestamp, r.battVoltage, r.battCurrent, r.battTemp, r.battSOC, r.battSOH,
      r.solarVoltage, r.solarPower, r.state, r.mode, r.fault);
    logInfo("DATALOGGER", line);
  }
}

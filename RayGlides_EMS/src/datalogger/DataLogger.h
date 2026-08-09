#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <Arduino.h>
#include "../battery/BatteryMonitor.h"
#include "../solar/SolarMonitor.h"
#include "../battery/BatteryStateMachine.h"
#include "../charging/ChargingDecision.h"
#include "../fault/FaultDetection.h"

struct LogRecord {
  unsigned long timestamp;
  float battVoltage, battCurrent, battTemp;
  int battSOC, battSOH;
  float solarVoltage, solarPower;
  uint8_t state;
  uint8_t mode;
  uint8_t fault;
};

// Initializes EEPROM storage and attempts to restore SOC/SOH estimator
// state from a previous session. Call once from setup(), AFTER the
// estimators themselves would otherwise start fresh (this restores over
// their default startup state if valid data is found).
void initDataLogger();

// Call once per main loop with the current full system snapshot. Two
// independent timers inside this function throttle the actual work:
//  - a RECORD is appended to the RAM history buffer every
//    DATALOGGER_RECORD_INTERVAL_MS (not every call)
//  - estimator state is checkpointed to EEPROM every
//    DATALOGGER_CHECKPOINT_INTERVAL_MS (a real flash write - kept
//    infrequent on purpose)
void updateDataLogger(BatteryData battery, SolarData solar, ChargeState state,
                       ChargingMode mode, FaultCode fault);

int getLogRecordCount();
LogRecord getLogRecord(int indexFromNewest);  // 0 = most recent
void printDataLog();                            // Dumps the RAM history via DebugLog
void clearDataLog();                             // Clears RAM history only (not the EEPROM checkpoint)

#endif

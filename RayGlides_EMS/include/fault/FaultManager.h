#ifndef FAULT_MANAGER_H
#define FAULT_MANAGER_H

#include <Arduino.h>

enum FaultCode {
  FAULT_NONE                  = 0,
  F001_BATTERY_NOT_DETECTED   = 1,
  F002_BATTERY_OVER_VOLTAGE    = 2,
  F003_BATTERY_UNDER_VOLTAGE   = 3,
  F004_BATTERY_OVER_TEMPERATURE = 4,
  F005_SOLAR_OVER_VOLTAGE     = 5,
  F006_SOLAR_REVERSE_POLARITY = 6,
  F007_MPPT_OVER_TEMPERATURE  = 7,
  F008_COMM_TIMEOUT           = 8,
  F009_CHECKSUM_ERROR         = 9,
  F010_SENSOR_FAILURE         = 10,
  F011_OVER_CURRENT           = 11,
  F012_THERMAL_SHUTDOWN       = 12,
  F013_WATCHDOG_LOCKOUT       = 13,
  F014_WATCHDOG_RESET         = 14
};

#define F012_WATCHDOG_RESET F014_WATCHDOG_RESET

enum Severity { 
  SEV_WARNING  = 0, 
  SEV_CRITICAL = 1 
};

struct FaultLogEntry {
  uint32_t timestampMs;
  uint8_t faultCode;
  uint8_t severity;
  bool active;
};

// Global fault variables
extern FaultCode currentPrimaryFault;
extern Severity currentPrimarySeverity;

// Initialize Central Fault Manager
void initFaultManager();

// Registers a fault condition
void triggerFault(FaultCode code, Severity severity);

// Clears/deactivates a specific fault
void clearFault(FaultCode code);

// Clears all currently active faults
void clearAllFaults();

// Check if a specific fault code is currently active
bool isFaultActive(FaultCode code);

// Check if any critical faults are active
bool hasCriticalFault();

// Get the name string of a fault code
const char* getFaultName(FaultCode code);

// Dumps the saved fault history logs to serial
void printFaultHistory();

#endif // FAULT_MANAGER_H

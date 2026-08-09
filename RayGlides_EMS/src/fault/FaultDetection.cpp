#include "FaultDetection.h"

const char* faultName(FaultCode f) {
  return getFaultName(f);
}

bool isCriticalFault(FaultCode f) {
  if (f == FAULT_NONE) return false;
  // Critical faults are: Battery Not Detected, Over Voltage, Under Voltage, Over Temp, Over Current, Thermal Shutdown, Watchdog Lockout
  return (f == F001_BATTERY_NOT_DETECTED || f == F002_BATTERY_OVER_VOLTAGE ||
          f == F003_BATTERY_UNDER_VOLTAGE || f == F004_BATTERY_OVER_TEMPERATURE ||
          f == F011_OVER_CURRENT || f == F012_THERMAL_SHUTDOWN ||
          f == F013_WATCHDOG_LOCKOUT);
}

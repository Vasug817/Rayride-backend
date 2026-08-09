#ifndef BATTERY_PROTECTION_H
#define BATTERY_PROTECTION_H

#include <Arduino.h>
#include "sensors/SensorInterface.h"
#include "fault/FaultManager.h"

struct ProtectionStatus {
  bool overVoltageFault;
  bool underVoltageFault;
  bool overCurrentFault;
  bool overTempFault;
  bool chargeDisable;
  bool loadDisable;
};

// Global protection state
extern ProtectionStatus currentProtection;

// Initialize protections
void initBatteryProtection();

// Run protection checks against system configuration thresholds and update status
ProtectionStatus checkBatteryProtection(SensorReading readings, unsigned long timeMs);

// Helper status checks
bool isChargeAllowed();
bool isLoadAllowed();

#endif // BATTERY_PROTECTION_H

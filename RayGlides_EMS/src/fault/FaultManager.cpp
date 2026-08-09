#include "fault/FaultManager.h"
#include <Preferences.h>
#include "debug/DebugLog.h"
#include "communication/MQTTComm.h"

#define MAX_ACTIVE_FAULTS 16
#define MAX_HISTORY_ENTRIES 10

static FaultLogEntry activeFaults[MAX_ACTIVE_FAULTS];
static int activeFaultCount = 0;

static Preferences faultPrefs;
FaultCode currentPrimaryFault = FAULT_NONE;
Severity currentPrimarySeverity = SEV_WARNING;

// Forward declarations of local helper functions
static void evaluatePrimaryFault();
static void logFaultHistory(FaultCode code, Severity severity);

void initFaultManager() {
  activeFaultCount = 0;
  currentPrimaryFault = FAULT_NONE;
  currentPrimarySeverity = SEV_WARNING;
  logInfo("FAULT", "Fault Manager Initialized.");
}

void triggerFault(FaultCode code, Severity severity) {
  if (code == FAULT_NONE) return;

  // Check if already active
  for (int i = 0; i < activeFaultCount; i++) {
    if (activeFaults[i].faultCode == code) {
      activeFaults[i].severity = severity;
      return;
    }
  }

  // Register new active fault
  if (activeFaultCount < MAX_ACTIVE_FAULTS) {
    activeFaults[activeFaultCount].timestampMs = millis();
    activeFaults[activeFaultCount].faultCode = code;
    activeFaults[activeFaultCount].severity = severity;
    activeFaults[activeFaultCount].active = true;
    activeFaultCount++;
    
    char msg[64];
    snprintf(msg, sizeof(msg), "Fault Triggered: %s (Severity: %s)", 
      getFaultName(code), severity == SEV_CRITICAL ? "CRITICAL" : "WARNING");
    logError("FAULT", msg);

    // Publish to MQTT
    publishFaultJSON(code, getFaultName(code), severity);

    // Send to Serial Protocol link
    extern void sendFaultReport(FaultCode code, Severity sev);
    sendFaultReport(code, severity);

    // Save to NVS history if critical
    if (severity == SEV_CRITICAL) {
      logFaultHistory(code, severity);
    }

    // Re-evaluate primary fault
    evaluatePrimaryFault();
  }
}

void clearFault(FaultCode code) {
  if (code == FAULT_NONE) return;

  for (int i = 0; i < activeFaultCount; i++) {
    if (activeFaults[i].faultCode == code) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Fault Cleared: %s", getFaultName(code));
      logInfo("FAULT", msg);

      // Publish to MQTT (severity -1 means cleared)
      publishFaultJSON(code, getFaultName(code), -1);

      // Send to Serial Protocol link
      extern void sendFaultReport(FaultCode code, Severity sev);
      sendFaultReport(code, (Severity)255);

      // Shift remaining faults down
      for (int j = i; j < activeFaultCount - 1; j++) {
        activeFaults[j] = activeFaults[j + 1];
      }
      activeFaultCount--;
      
      // Re-evaluate primary fault
      evaluatePrimaryFault();
      return;
    }
  }
}

void clearAllFaults() {
  extern void clearWatchdogLockout();
  clearWatchdogLockout();
  
  if (activeFaultCount > 0) {
    logInfo("FAULT", "Clearing all active faults.");
    extern void sendFaultReport(FaultCode code, Severity sev);
    for (int i = 0; i < activeFaultCount; i++) {
      publishFaultJSON(activeFaults[i].faultCode, getFaultName((FaultCode)activeFaults[i].faultCode), -1);
      sendFaultReport((FaultCode)activeFaults[i].faultCode, (Severity)255);
    }
    activeFaultCount = 0;
    currentPrimaryFault = FAULT_NONE;
    currentPrimarySeverity = SEV_WARNING;
  }
}

bool isFaultActive(FaultCode code) {
  for (int i = 0; i < activeFaultCount; i++) {
    if (activeFaults[i].faultCode == code) {
      return true;
    }
  }
  return false;
}

bool hasCriticalFault() {
  for (int i = 0; i < activeFaultCount; i++) {
    if (activeFaults[i].severity == SEV_CRITICAL) {
      return true;
    }
  }
  return false;
}

static void evaluatePrimaryFault() {
  if (activeFaultCount == 0) {
    currentPrimaryFault = FAULT_NONE;
    currentPrimarySeverity = SEV_WARNING;
    return;
  }

  int primaryIdx = 0;
  for (int i = 1; i < activeFaultCount; i++) {
    if (activeFaults[i].severity > activeFaults[primaryIdx].severity) {
      primaryIdx = i;
    } else if (activeFaults[i].severity == activeFaults[primaryIdx].severity) {
      if (activeFaults[i].timestampMs > activeFaults[primaryIdx].timestampMs) {
        primaryIdx = i;
      }
    }
  }

  currentPrimaryFault = (FaultCode)activeFaults[primaryIdx].faultCode;
  currentPrimarySeverity = (Severity)activeFaults[primaryIdx].severity;
}

static void logFaultHistory(FaultCode code, Severity severity) {
  faultPrefs.begin("faulthist", false);
  
  int count = faultPrefs.getInt("count", 0);
  int nextIdx = count % MAX_HISTORY_ENTRIES;

  char key_code[16], key_sev[16], key_time[16];
  snprintf(key_code, sizeof(key_code), "code_%d", nextIdx);
  snprintf(key_sev, sizeof(key_sev), "sev_%d", nextIdx);
  snprintf(key_time, sizeof(key_time), "time_%d", nextIdx);

  faultPrefs.putInt(key_code, code);
  faultPrefs.putInt(key_sev, severity);
  faultPrefs.putUInt(key_time, millis());

  faultPrefs.putInt("count", count + 1);
  faultPrefs.end();
}

void printFaultHistory() {
  faultPrefs.begin("faulthist", true);
  int count = faultPrefs.getInt("count", 0);
  logInfo("FAULT", "--- Persisted Critical Fault History ---");
  int displayCount = count > MAX_HISTORY_ENTRIES ? MAX_HISTORY_ENTRIES : count;
  
  for (int i = 0; i < displayCount; i++) {
    int idx = (count > MAX_HISTORY_ENTRIES) ? ((count + i) % MAX_HISTORY_ENTRIES) : i;
    char key_code[16], key_sev[16], key_time[16];
    snprintf(key_code, sizeof(key_code), "code_%d", idx);
    snprintf(key_sev, sizeof(key_sev), "sev_%d", idx);
    snprintf(key_time, sizeof(key_time), "time_%d", idx);

    int code = faultPrefs.getInt(key_code, 0);
    int sev = faultPrefs.getInt(key_sev, 0);
    uint32_t timestamp = faultPrefs.getUInt(key_time, 0);

    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), "[%d] Code: %s, Severity: %s, Time: %u ms", 
      i, getFaultName((FaultCode)code), sev == SEV_CRITICAL ? "CRITICAL" : "WARNING", timestamp);
    logInfo("FAULT", logMsg);
  }
  faultPrefs.end();
}

const char* getFaultName(FaultCode code) {
  switch (code) {
    case FAULT_NONE:                  return "None";
    case F001_BATTERY_NOT_DETECTED:   return "F001 Battery Not Detected";
    case F002_BATTERY_OVER_VOLTAGE:    return "F002 Battery Over-Voltage";
    case F003_BATTERY_UNDER_VOLTAGE:   return "F003 Battery Under-Voltage";
    case F004_BATTERY_OVER_TEMPERATURE: return "F004 Battery Over-Temperature";
    case F005_SOLAR_OVER_VOLTAGE:     return "F005 Solar Over-Voltage";
    case F006_SOLAR_REVERSE_POLARITY: return "F006 Solar Reverse Polarity";
    case F007_MPPT_OVER_TEMPERATURE:  return "F007 MPPT Over-Temperature";
    case F008_COMM_TIMEOUT:           return "F008 Comm Timeout";
    case F009_CHECKSUM_ERROR:         return "F009 Checksum Error";
    case F010_SENSOR_FAILURE:         return "F010 Sensor Failure";
    case F011_OVER_CURRENT:           return "F011 Battery Over-Current";
    case F012_THERMAL_SHUTDOWN:       return "F012 Thermal Shutdown";
    case F013_WATCHDOG_LOCKOUT:       return "F013 Watchdog Lockout";
    case F014_WATCHDOG_RESET:         return "F014 Watchdog Reset Recovered";
    default:                          return "Unknown Fault";
  }
}

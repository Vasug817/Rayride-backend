// RayGlides Smart EMS - Main Firmware Entry Point
// Wires together battery monitoring, solar monitoring, fault detection,
// relay control, charging decision, and the communication module.
// Uses a cooperative Task Scheduler to manage task frequencies.

#include <Arduino.h>
#include "config.h"

// Phase 1 Modular Architecture Headers
#include "configuration/ConfigManager.h"
#include "sensors/SensorInterface.h"
#include "sensors/SensorSimulator.h"
#include "protection/BatteryProtection.h"
#include "fault/FaultManager.h"
#include "thermal/ThermalManager.h"
#include "energy/EnergyEngine.h"
#include "analytics/BatteryAnalytics.h"

#include "battery/BatteryMonitor.h"
#include "battery/BatteryStateMachine.h"
#include "solar/SolarMonitor.h"
#include "mppt/MPPTAlgorithm.h"
#include "fault/FaultDetection.h"
#include "fault/FaultLog.h"
#include "datalogger/DataLogger.h"
#include "charging/ChargingDecision.h"
#include "relay/RelayControl.h"
#include "protocol/RayGlidesProtocol.h"
#include "can/CANComm.h"
#include "rs485/RS485Comm.h"
#include "debug/DebugLog.h"
#include "watchdog/WatchdogRecovery.h"
#include "ota/OTAUpdate.h"

// New system modules
#include "scheduler/Scheduler.h"
#include "wifi/WiFiManager.h"
#include "power/PowerOptimization.h"
#include "communication/MQTTComm.h"
#include "communication/OfflineBuffer.h"

MPPTMethod activeMPPTMethod = MPPT_PERTURB_OBSERVE;
ChargeState currentState = STATE_BOOT;
FaultCode activeFault = FAULT_NONE;

// Shared global state variables accessed across tasks
BatteryData battery;
SolarData solar;
ChargingMode mode;
ChargingMode manualModeOverride = MODE_SOLAR_ONLY;
bool useManualMode = false;
MPPTResult mppt;

// Forward declarations of task callbacks
void runReadSensorsTask();
void runTelemetryTask();
void runWiFiTask();
void runPowerTask();
void runDataLogTask();
void pollCommunications();

void setup() {
  Serial.begin(115200);
  delay(1000);

  initDebugLog();
  setDebugEnabled(DEBUG_ENABLED_DEFAULT);

  // Initialize NVS configuration manager first
  initConfig();

  // Initialize unified Sensor Abstraction Layer (SAL)
  initSensors();

  // Initialize new modules
  initOfflineBuffer();
  initBatteryProtection();
  initFaultManager();
  initThermalManager();
  initEnergyEngine();
  initBatteryAnalytics();

  pinMode(GRID_BUTTON_PIN, INPUT_PULLDOWN);
  initRelay();                                 // Relay defaults open FIRST
  uint8_t wdtBootFault = initWatchdogRecovery(); // Then start hardware WDT protection, ASAP
  initOTARecovery();                             // Roll back to the previous image if the last OTA never confirmed healthy
  noteMessageReceived();                         // Start the comm watchdog clock

  ledcSetup(MPPT_PWM_CHANNEL, MPPT_PWM_FREQ_HZ, MPPT_PWM_RESOLUTION_BITS);
  ledcAttachPin(MPPT_PWM_PIN, MPPT_PWM_CHANNEL);
  initMPPT(0.5);

  initCAN();
  initRS485();

  printFaultLog();  // Show any critical fault history carried over from before reboot
  printFaultHistory();

  if (wdtBootFault != FAULT_NONE) {
    FaultCode f = (FaultCode)wdtBootFault;
    Severity sev = isCriticalFault(f) ? SEV_CRITICAL : SEV_WARNING;
    triggerFault(f, sev);
  }

  initWiFi();
  initMQTT();
  initPowerOptimization();
  initScheduler();

  // Register cooperative tasks
  addTask("ReadSensors", 200, runReadSensorsTask);
  addTask("Telemetry", 500, runTelemetryTask);
  addTask("WiFiUpdate", 1000, runWiFiTask);
  addTask("PowerOpt", 2000, runPowerTask);
  addTask("DataLog", 10000, runDataLogTask);

  logInfo("MAIN", "RayGlides EMS Firmware Starting...");
}

void loop() {
  // Listen for incoming frames continuously (must be run on every loop to be responsive)
  pollCommunications();

  // Run the cooperative task scheduler
  runScheduler();

  // Watchdog: confirm stability once, then feed every cycle
  static bool wdtHealthConfirmed = false;
  if (!wdtHealthConfirmed && millis() >= WATCHDOG_HEALTHY_UPTIME_MS) {
    confirmWatchdogHealthy();
    confirmOTAHealthy();
    wdtHealthConfirmed = true;
  }
  feedWatchdog();

  // Tiny sleep to yield CPU and prevent 100% single-core utilization
  delay(1);
}

// --- Task Implementations ---

void runReadSensorsTask() {
  // 1. Read battery, solar, and temp values from SAL
  SensorReading readings = readSensors();

  // Map to global models for telemetry/compatibility
  battery.voltage = readings.batteryVoltage;
  battery.current = readings.batteryCurrent;
  battery.temperature = readings.batteryTemp;
  battery.soh = getAnalyticsSOH();
  battery.soc = getAnalyticsSOC();

  solar.voltage = readings.solarVoltage;
  solar.current = readings.solarCurrent;
  solar.power = readings.solarVoltage * readings.solarCurrent;

  // 2. Check protections (BPS)
  ProtectionStatus prot = checkBatteryProtection(readings, millis());

  // 3. Centralized Fault Management mapping
  // Map sensor health failures
  if (!readings.batteryVoltageHealthy || !readings.batteryCurrentHealthy || !readings.batteryTempHealthy ||
      !readings.solarVoltageHealthy || !readings.solarCurrentHealthy || !readings.mpptTempHealthy) {
    triggerFault(F010_SENSOR_FAILURE, SEV_CRITICAL);
  } else {
    clearFault(F010_SENSOR_FAILURE);
  }

  // Map protection triggers to faults
  if (prot.overVoltageFault)       triggerFault(F002_BATTERY_OVER_VOLTAGE, SEV_CRITICAL);
  else                             clearFault(F002_BATTERY_OVER_VOLTAGE);

  if (prot.underVoltageFault)      triggerFault(F003_BATTERY_UNDER_VOLTAGE, SEV_CRITICAL);
  else                             clearFault(F003_BATTERY_UNDER_VOLTAGE);

  if (prot.overCurrentFault)       triggerFault(F011_OVER_CURRENT, SEV_CRITICAL);
  else                             clearFault(F011_OVER_CURRENT);

  if (prot.overTempFault)          triggerFault(F004_BATTERY_OVER_TEMPERATURE, SEV_CRITICAL);
  else                             clearFault(F004_BATTERY_OVER_TEMPERATURE);

  // Map Solar Plausibility Faults
  SystemConfig cfg = getSystemConfig();
  if (readings.solarCurrent > 0.3f && readings.solarVoltage < 1.0f) {
    triggerFault(F006_SOLAR_REVERSE_POLARITY, SEV_WARNING);
  } else {
    clearFault(F006_SOLAR_REVERSE_POLARITY);
  }

  if (readings.solarVoltage > cfg.solarVoltageLimit) {
    triggerFault(F005_SOLAR_OVER_VOLTAGE, SEV_WARNING);
  } else {
    clearFault(F005_SOLAR_OVER_VOLTAGE);
  }

  // Communication Timeout Check
#if SIMULATE_SENSORS
  noteMessageReceived();  // Prevent timeout in simulator modes
#endif
  if (checkCommunicationTimeout()) {
    triggerFault(F008_COMM_TIMEOUT, SEV_WARNING);
  } else {
    clearFault(F008_COMM_TIMEOUT);
  }

  // Sync primary fault global
  activeFault = currentPrimaryFault;
  bool criticalFault = hasCriticalFault();

  // 4. Update Charging State Machine v2
  ChargeState nextState = evaluateBatteryState(currentState, readings.batteryVoltage, readings.solarVoltage, getAnalyticsSOC(), criticalFault, millis());
  if (nextState != currentState) {
    char msg[48];
    snprintf(msg, sizeof(msg), "%s -> %s", stateName(currentState), stateName(nextState));
    logInfo("BATTERY", msg);
    currentState = nextState;
  }

  // 5. Relay Control
  updateRelay(currentState, activeFault);

  // Turn PWM on/off based on state
  if (currentState == STATE_CHARGING && !prot.chargeDisable && !isThermalShutdownActive()) {
    if (!isPWMEnabled()) {
      enablePWM();
    }
  } else {
    if (isPWMEnabled()) {
      disablePWM();
    }
  }

  // 6. Thermal Fan Regulation and Power Derating
  updateThermalManager(readings.mpptTemp, millis());

  // 7. Charging Mode selection
  bool gridAvailable = digitalRead(GRID_BUTTON_PIN) == HIGH;
  if (useManualMode) {
    mode = manualModeOverride;
  } else {
    mode = decideChargingMode(solar, gridAvailable);
  }

  // 8. Run MPPT algorithm converter
  mppt = updateMPPT(activeMPPTMethod, solar, activeFault, millis());

  // 9. Update Energy accumulator calculations
  updateEnergyEngine(readings.batteryVoltage, readings.batteryCurrent, readings.solarVoltage, readings.solarCurrent, millis());

  // 10. Update Battery Analytics (coulomb count and SOH)
  updateBatteryAnalytics(readings.batteryVoltage, readings.batteryCurrent, readings.batteryTemp, millis());
}

void runTelemetryTask() {
  // Report battery and solar telemetry on CAN and RS485 / Serial
  sendCANStatus(battery, solar, currentState);
  
  if (!isOTAInProgress()) {
    sendStatusUpdate(battery, solar, currentState);
    uint8_t statusPayload[8] = {
      (uint8_t)getAnalyticsSOC(),
      (uint8_t)currentState,
      (uint8_t)constrain((int)round(battery.voltage), 0, 255),
      (uint8_t)(int8_t)constrain((int)round(battery.current), -128, 127),
      (uint8_t)constrain((int)round(battery.temperature), 0, 255),
      (uint8_t)constrain((int)round(solar.voltage), 0, 255),
      (uint8_t)constrain((int)round(solar.power), 0, 255),
      (uint8_t)constrain(getAnalyticsSOH(), 0, 255)
    };
    rs485SendFrame(MSG_STATUS_UPDATE, statusPayload, 8);
  }

  // Publish to MQTT JSON
  publishTelemetryJSON(
    battery.voltage, battery.current, battery.temperature, getAnalyticsSOC(), getAnalyticsSOH(),
    solar.voltage, solar.power, getFanDuty(),
    getAccumulatedSolarWh(), getAccumulatedChargingWh(), getAccumulatedConsumedWh(), getAccumulatedAh(),
    (uint8_t)currentState, (uint8_t)mode, getFaultName(activeFault)
  );

  // Debug output
  char telemetry[256];
  snprintf(telemetry, sizeof(telemetry),
    "BattV=%.1f I=%.1f T=%.1f SOC=%d%% SOH=%d%% | SolarV=%.1f P=%.1fW Duty=%d%% Fan=%d%% | SolarWh=%.2f ChgWh=%.2f ConsWh=%.2f Ah=%.2f | State=%s Mode=%s Fault=%s",
    battery.voltage, battery.current, battery.temperature, getAnalyticsSOC(), getAnalyticsSOH(),
    solar.voltage, solar.power, (int)(mppt.dutyCycle * 100), (int)(getFanDuty() * 100),
    getAccumulatedSolarWh(), getAccumulatedChargingWh(), getAccumulatedConsumedWh(), getAccumulatedAh(),
    stateName(currentState), modeName(mode), getFaultName(activeFault));
  logInfo("TELEMETRY", telemetry);
}

void runWiFiTask() {
  updateWiFi();
  updateMQTT(millis());
}

void runPowerTask() {
  optimizePowerState(currentState, solar);
}

void runDataLogTask() {
  updateDataLogger(battery, solar, currentState, mode, activeFault);
}

void pollCommunications() {
  // Poll Serial
  ReceivedFrame incoming = receiveFrame();
  if (incoming.valid && isOTAMessage(incoming.msgId)) {
    handleOTAFrame(incoming, sendFrame);
  } else {
    handleIncomingFrame(incoming);
  }

  // Poll CAN
  CANReceivedMessage canIncoming = receiveCAN();
  if (canIncoming.valid) {
    char msg[48];
    snprintf(msg, sizeof(msg), "RX CAN ID 0x%03X len=%d", (unsigned int)canIncoming.id, canIncoming.length);
    logInfo("CAN", msg);
    noteMessageReceived();
  }

  // Poll RS485
  ReceivedFrame rs485Incoming = rs485ReceiveFrame();
  if (rs485Incoming.valid && isOTAMessage(rs485Incoming.msgId)) {
    handleOTAFrame(rs485Incoming, rs485SendFrame);
    noteMessageReceived();
  } else if (rs485Incoming.valid) {
    char msg[40];
    snprintf(msg, sizeof(msg), "RX RS485 MSG_ID 0x%02X", rs485Incoming.msgId);
    logInfo("RS485", msg);
    noteMessageReceived();
  }
}

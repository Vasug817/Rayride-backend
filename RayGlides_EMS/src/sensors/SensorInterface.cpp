#include "sensors/SensorInterface.h"
#include "sensors/SensorSimulator.h"
#include "config.h"

void initSensors() {
  initSensorSimulator();
  // Pin modes for physical hardware ADCs
  pinMode(BATTERY_VOLTAGE_PIN, INPUT);
  pinMode(BATTERY_CURRENT_PIN, INPUT);
  pinMode(BATTERY_TEMP_PIN, INPUT);
  pinMode(SOLAR_VOLTAGE_PIN, INPUT);
  pinMode(SOLAR_CURRENT_PIN, INPUT);
}

SensorReading readSensors() {
#if SIMULATE_SENSORS
  return getSimulatedReading(millis());
#else
  SensorReading r;
  r.batteryVoltageHealthy = true;
  r.batteryCurrentHealthy = true;
  r.batteryTempHealthy = true;
  r.solarVoltageHealthy = true;
  r.solarCurrentHealthy = true;
  r.mpptTempHealthy = true;
  r.overallHealthy = true;

  // Read Battery Voltage (ADC1)
  int rawV = analogRead(BATTERY_VOLTAGE_PIN);
  float vSens = (rawV / ADC_MAX) * ADC_VREF;
  r.batteryVoltage = vSens * VOLTAGE_DIVIDER_RATIO;
  if (rawV < DETECT_MIN_RAW) {
    r.batteryVoltageHealthy = false;
    r.overallHealthy = false;
  }

  // Read Battery Current
  int rawI = analogRead(BATTERY_CURRENT_PIN);
  float vI = (rawI / ADC_MAX) * ADC_VREF;
  r.batteryCurrent = (vI - CURRENT_SENSOR_MIDPOINT) / CURRENT_SENSITIVITY_V_PER_A;

  // Read Battery Temp
  int rawT = analogRead(BATTERY_TEMP_PIN);
  float vT = (rawT / ADC_MAX) * ADC_VREF;
  r.batteryTemp = (vT * 1000.0f) / TEMP_MV_PER_C;

  // Read Solar Voltage
  int rawSolV = analogRead(SOLAR_VOLTAGE_PIN);
  float vSolSens = (rawSolV / ADC_MAX) * ADC_VREF;
  r.solarVoltage = vSolSens * SOLAR_VOLTAGE_DIVIDER_RATIO;
  if (rawSolV >= SOLAR_MAX_PLAUSIBLE_RAW) {
    r.solarVoltageHealthy = false;
    r.overallHealthy = false;
  }

  // Read Solar Current
  int rawSolI = analogRead(SOLAR_CURRENT_PIN);
  float vSolI = (rawSolI / ADC_MAX) * ADC_VREF;
  r.solarCurrent = (vSolI - CURRENT_SENSOR_MIDPOINT) / SOLAR_CURRENT_SENSITIVITY_V_PER_A;

  // MPPT Enclosure / Heatsink temp
  r.mpptTemp = r.batteryTemp + 3.0f; 

  return r;
#endif
}

#ifndef SENSOR_INTERFACE_H
#define SENSOR_INTERFACE_H

#include <Arduino.h>

struct SensorReading {
  float batteryVoltage;
  float batteryCurrent;
  float batteryTemp;
  float solarVoltage;
  float solarCurrent;
  float mpptTemp;
  bool batteryVoltageHealthy;
  bool batteryCurrentHealthy;
  bool batteryTempHealthy;
  bool solarVoltageHealthy;
  bool solarCurrentHealthy;
  bool mpptTempHealthy;
  bool overallHealthy;
};

// Initialize sensor hardware and pins
void initSensors();

// Perform a single read cycle across all active interfaces (both simulated or physical ADC)
SensorReading readSensors();

#endif // SENSOR_INTERFACE_H

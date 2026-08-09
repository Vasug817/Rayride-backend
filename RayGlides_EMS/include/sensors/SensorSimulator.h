#ifndef SENSOR_SIMULATOR_H
#define SENSOR_SIMULATOR_H

#include <Arduino.h>
#include "sensors/SensorInterface.h"

// Scenario enumerations matching the Day 44 specifications
enum SimulationScenario {
  SCENARIO_NORMAL_OPERATION = 0,
  SCENARIO_CHARGING          = 1,
  SCENARIO_LOW_BATTERY       = 2,
  SCENARIO_FULL_BATTERY      = 3,
  SCENARIO_HIGH_TEMPERATURE  = 4,
  SCENARIO_HIGH_CURRENT      = 5,
  SCENARIO_NO_SOLAR          = 6,
  SCENARIO_SENSOR_FAILURE    = 7
};

// Initialize simulator values
void initSensorSimulator();

// Set the active simulation scenario
void setSimulationScenario(uint8_t scenarioId);

// Get the active simulation scenario ID
uint8_t getSimulationScenario();

// Get the name string of a scenario
const char* getScenarioName(uint8_t scenarioId);

// Generate simulated sensor reading based on active scenario and runtime timestamp
SensorReading getSimulatedReading(unsigned long timeMs);

#endif // SENSOR_SIMULATOR_H

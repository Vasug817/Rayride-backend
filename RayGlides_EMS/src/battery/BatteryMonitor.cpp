#include "BatteryMonitor.h"
#include "config.h"
#include "SOCEstimator.h"
#include "SOHEstimator.h"

BatteryData readBatteryData() {
  BatteryData data;

#if SIMULATE_SENSORS
  data.voltage = 52.0;       // Healthy ~52V battery pack
  data.current = 2.5;        // Charging current
  data.temperature = 25.0;   // Healthy room temp (25 C)
  data.rawVoltage = (int)((52.0 / VOLTAGE_DIVIDER_RATIO) / ADC_VREF * ADC_MAX);
#else
  // --- Voltage (through the 200k/10k divider) ---
  data.rawVoltage = analogRead(BATTERY_VOLTAGE_PIN);
  float dividerOutputVoltage = (data.rawVoltage / ADC_MAX) * ADC_VREF;
  data.voltage = dividerOutputVoltage * VOLTAGE_DIVIDER_RATIO;

  // --- Current (ACS712-style: centered at CURRENT_SENSOR_MIDPOINT for 0A) ---
  int rawCurrent = analogRead(BATTERY_CURRENT_PIN);
  float currentSensorVoltage = (rawCurrent / ADC_MAX) * ADC_VREF;
  data.current = (currentSensorVoltage - CURRENT_SENSOR_MIDPOINT) / CURRENT_SENSITIVITY_V_PER_A;

  // --- Temperature (LM35-style: TEMP_MV_PER_C millivolts per degree C) ---
  int rawTemp = analogRead(BATTERY_TEMP_PIN);
  float tempSensorVoltage = (rawTemp / ADC_MAX) * ADC_VREF;
  data.temperature = (tempSensorVoltage * 1000.0) / TEMP_MV_PER_C;
#endif

  // --- State of charge: hybrid OCV + Coulomb-counting estimate, not a
  // direct raw-ADC percentage (see SOCEstimator.h/.cpp) ---
  data.soc = (int)round(estimateSOC_Hybrid(data.voltage, data.current));
  data.soc = constrain(data.soc, 0, 100);

  // --- State of health: internal resistance + cycle-count degradation
  // estimate, distinct from SOC (see SOHEstimator.h/.cpp) ---
  data.soh = estimateSOH(data.voltage, data.current);

  return data;
}

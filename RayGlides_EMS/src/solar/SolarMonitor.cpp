#include "SolarMonitor.h"
#include "config.h"

SolarData readSolarData() {
  SolarData data;

#if SIMULATE_SENSORS
  data.voltage = 24.0;       // Healthy solar voltage (within 26.4V limit of the 8.0 divider)
  data.current = 8.0;        // 8.0A of solar current
  data.rawVoltage = (int)((24.0 / SOLAR_VOLTAGE_DIVIDER_RATIO) / ADC_VREF * ADC_MAX);
  data.rawCurrent = (int)(((8.0 * SOLAR_CURRENT_SENSITIVITY_V_PER_A) + CURRENT_SENSOR_MIDPOINT) / ADC_VREF * ADC_MAX);
#else
  // --- Voltage (through the solar panel voltage divider) ---
  data.rawVoltage = analogRead(SOLAR_VOLTAGE_PIN);
  float dividerOutputVoltage = (data.rawVoltage / ADC_MAX) * ADC_VREF;
  data.voltage = dividerOutputVoltage * SOLAR_VOLTAGE_DIVIDER_RATIO;

  // --- Current (ACS712 5A-style: centered at CURRENT_SENSOR_MIDPOINT for 0A) ---
  data.rawCurrent = analogRead(SOLAR_CURRENT_PIN);
  float currentSensorVoltage = (data.rawCurrent / ADC_MAX) * ADC_VREF;
  data.current = (currentSensorVoltage - CURRENT_SENSOR_MIDPOINT) / SOLAR_CURRENT_SENSITIVITY_V_PER_A;
  if (data.current < 0) data.current = 0;  // Solar panels don't sink current in this model
#endif

  // --- Power (derived) ---
  data.power = data.voltage * data.current;

  return data;
}

#ifndef MQTT_COMM_H
#define MQTT_COMM_H

#include <Arduino.h>

// Initialize Wi-Fi connection and PubSubClient settings
void initMQTT();

// Maintain Wi-Fi and MQTT connection states (reconnections)
void updateMQTT(unsigned long nowMs);

// Publish structured JSON telemetry packet to "ems/telemetry"
void publishTelemetryJSON(
  float battV, float battI, float temp, int soc, int soh, 
  float solarV, float solarP, float fanDuty, 
  float solarWh, float chgWh, float consWh, float netAh, 
  uint8_t state, uint8_t mode, const char* fault
);

// Publish active fault JSON to "ems/faults"
void publishFaultJSON(int faultCode, const char* faultName, int severity);

// Check if MQTT client is currently connected
bool isMQTTConnected();

#endif // MQTT_COMM_H

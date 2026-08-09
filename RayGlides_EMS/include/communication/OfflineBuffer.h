#ifndef OFFLINE_BUFFER_H
#define OFFLINE_BUFFER_H

#include <Arduino.h>

struct TelemetryRecord {
  uint32_t timestampMs;
  uint32_t sequenceNum;
  uint8_t soc;
  uint8_t state;
  float battV;
  float battI;
  float temp;
  float solarV;
  float solarP;
};

void initOfflineBuffer();
void bufferTelemetry(uint32_t seqNum, uint8_t soc, uint8_t state, float battV, float battI, float temp, float solarV, float solarP);
bool hasBufferedTelemetry();
bool getNextBufferedTelemetry(TelemetryRecord& record);
void popBufferedTelemetry();
uint32_t getBufferedCount();
void clearOfflineBuffer();

#endif // OFFLINE_BUFFER_H

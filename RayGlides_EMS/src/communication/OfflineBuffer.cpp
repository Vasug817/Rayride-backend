#include "communication/OfflineBuffer.h"
#include "debug/DebugLog.h"

#define MAX_BUFFERED_RECORDS 100

static TelemetryRecord buffer[MAX_BUFFERED_RECORDS];
static uint32_t head = 0;
static uint32_t tail = 0;
static uint32_t count = 0;

void initOfflineBuffer() {
  head = 0;
  tail = 0;
  count = 0;
}

void bufferTelemetry(uint32_t seqNum, uint8_t soc, uint8_t state, float battV, float battI, float temp, float solarV, float solarP) {
  // If the buffer is full, overwrite the oldest element (at the tail)
  if (count == MAX_BUFFERED_RECORDS) {
    tail = (tail + 1) % MAX_BUFFERED_RECORDS;
    count--;
  }

  buffer[head].timestampMs = millis();
  buffer[head].sequenceNum = seqNum;
  buffer[head].soc = soc;
  buffer[head].state = state;
  buffer[head].battV = battV;
  buffer[head].battI = battI;
  buffer[head].temp = temp;
  buffer[head].solarV = solarV;
  buffer[head].solarP = solarP;

  head = (head + 1) % MAX_BUFFERED_RECORDS;
  count++;

  char dbg[64];
  snprintf(dbg, sizeof(dbg), "Telemetry buffered. Count: %d/%d", count, MAX_BUFFERED_RECORDS);
  logInfo("OFFLINE", dbg);
}

bool hasBufferedTelemetry() {
  return count > 0;
}

bool getNextBufferedTelemetry(TelemetryRecord& record) {
  if (count == 0) return false;
  record = buffer[tail];
  return true;
}

void popBufferedTelemetry() {
  if (count == 0) return;
  tail = (tail + 1) % MAX_BUFFERED_RECORDS;
  count--;
}

uint32_t getBufferedCount() {
  return count;
}

void clearOfflineBuffer() {
  head = 0;
  tail = 0;
  count = 0;
}

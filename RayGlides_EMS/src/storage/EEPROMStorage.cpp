#include "EEPROMStorage.h"
#include "config.h"
#include <EEPROM.h>
#include <string.h>

static uint32_t floatBits(float f) {
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  return bits;
}

static uint32_t computeStateChecksum(const PersistedEstimatorState &s) {
  // Simple sum-based checksum over the fields (not cryptographic - just
  // enough to detect an uninitialized/corrupted EEPROM region).
  uint32_t sum = s.magic;
  sum += floatBits(s.coulombSOC);
  sum += floatBits(s.sohResistance);
  sum += (uint32_t)s.sohCycleCount;
  sum += floatBits(s.sohThroughputAh);
  return sum;
}

void initEEPROMStorage() {
  EEPROM.begin(EEPROM_SIZE);
}

bool loadPersistedState(PersistedEstimatorState &out) {
  PersistedEstimatorState loaded;
  EEPROM.get(0, loaded);

  if (loaded.magic != EEPROM_STATE_MAGIC) {
    return false;  // Never written, or a different firmware version's layout
  }
  if (loaded.checksum != computeStateChecksum(loaded)) {
    return false;  // Corrupted (e.g. power lost mid-write)
  }

  out = loaded;
  return true;
}

void savePersistedState(const PersistedEstimatorState &state) {
  PersistedEstimatorState toSave = state;
  toSave.magic = EEPROM_STATE_MAGIC;
  toSave.checksum = computeStateChecksum(toSave);

  EEPROM.put(0, toSave);
  EEPROM.commit();
}

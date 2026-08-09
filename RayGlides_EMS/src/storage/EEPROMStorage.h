#ifndef EEPROM_STORAGE_H
#define EEPROM_STORAGE_H

#include <Arduino.h>

// Persisted battery estimator state - what needs to survive a reboot so
// SOC/SOH estimation continues from where it left off instead of
// resetting to "50% charged, brand new battery" every power-up.
struct PersistedEstimatorState {
  uint32_t magic;           // Marks the EEPROM region as containing valid data
  float coulombSOC;          // SOCEstimator's running Coulomb-counted value
  float sohResistance;       // SOHEstimator's running internal resistance estimate
  int32_t sohCycleCount;     // SOHEstimator's equivalent full-cycle count
  float sohThroughputAh;     // SOHEstimator's partial-cycle Coulomb throughput
  uint32_t checksum;          // Simple integrity check over the fields above
};

// Initializes the EEPROM emulation region. Call once from setup(),
// before any load/save calls.
void initEEPROMStorage();

// Loads persisted state into 'out'. Returns false (and leaves 'out'
// untouched) if the EEPROM region has never been written, or fails the
// magic/checksum check (e.g. first boot ever, or corrupted data).
bool loadPersistedState(PersistedEstimatorState &out);

// Saves state to EEPROM and commits it to flash. This is a real flash
// write - callers should throttle how often this is called (see
// DataLogger's checkpoint interval) rather than calling it every loop.
void savePersistedState(const PersistedEstimatorState &state);

#endif

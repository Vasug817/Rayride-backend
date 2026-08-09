#ifndef ENERGY_ENGINE_H
#define ENERGY_ENGINE_H

#include <Arduino.h>

// Initialize energy integration values
void initEnergyEngine();

// Perform integration on power and current curves using trapezoidal rules
void updateEnergyEngine(float battV, float battI, float solarV, float solarI, unsigned long timeMs);

// Retrieve accumulated solar energy (Wh)
float getAccumulatedSolarWh();

// Retrieve accumulated battery charging energy (Wh)
float getAccumulatedChargingWh();

// Retrieve accumulated battery consumed energy (Wh)
float getAccumulatedConsumedWh();

// Retrieve accumulated net battery ampere-hours (Ah)
float getAccumulatedAh();

// Resets all accumulators to 0
void resetEnergyStats();

#endif // ENERGY_ENGINE_H

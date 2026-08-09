#ifndef CHARGING_DECISION_H
#define CHARGING_DECISION_H

#include <Arduino.h>
#include "../solar/SolarMonitor.h"

enum ChargingMode { MODE_SOLAR_ONLY, MODE_GRID_ONLY, MODE_HYBRID, MODE_NO_CHARGE };

const char* modeName(ChargingMode m);

// Selects a charging mode from solar panel data (voltage/current/power)
// and grid availability.
ChargingMode decideChargingMode(SolarData solar, bool gridAvailable);

#endif

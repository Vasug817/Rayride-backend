#ifndef POWER_OPTIMIZATION_H
#define POWER_OPTIMIZATION_H

#include <Arduino.h>
#include "../solar/SolarMonitor.h"
#include "../battery/BatteryStateMachine.h"

enum PowerMode {
  POWER_PERFORMANCE,  // 240MHz CPU, modem active
  POWER_BALANCED,     // 160MHz CPU, modem sleep enabled
  POWER_SAVING,       // 80MHz CPU, modem sleep enabled
  POWER_SLEEP         // Light sleep
};

void initPowerOptimization();
void setPowerMode(PowerMode mode);
void optimizePowerState(ChargeState emsState, SolarData solar);

#endif

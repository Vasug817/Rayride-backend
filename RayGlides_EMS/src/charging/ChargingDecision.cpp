#include "ChargingDecision.h"
#include "config.h"

const char* modeName(ChargingMode m) {
  switch (m) {
    case MODE_SOLAR_ONLY: return "SOLAR_ONLY";
    case MODE_GRID_ONLY:  return "GRID_ONLY";
    case MODE_HYBRID:     return "HYBRID";
    case MODE_NO_CHARGE:  return "NO_CHARGE";
  }
  return "UNKNOWN";
}

ChargingMode decideChargingMode(SolarData solar, bool gridAvailable) {
  if (solar.power >= SOLAR_SUFFICIENT_W) {
    return MODE_SOLAR_ONLY;
  } else if (solar.power >= SOLAR_USABLE_W && gridAvailable) {
    return MODE_HYBRID;
  } else if (gridAvailable) {
    return MODE_GRID_ONLY;
  } else if (solar.power >= SOLAR_USABLE_W) {
    return MODE_SOLAR_ONLY;
  }
  return MODE_NO_CHARGE;
}

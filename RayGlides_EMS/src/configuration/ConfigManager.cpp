#include "configuration/ConfigManager.h"
#include <Preferences.h>
#include "debug/DebugLog.h"

SystemConfig sysConfig;
static Preferences prefs;

void initConfig() {
  prefs.begin("syscfg", false);
  
  if (!prefs.isKey("ov_v")) {
    logInfo("CONFIG", "No configuration found in NVS. Writing defaults...");
    sysConfig.batteryOverVoltage = 68.0f;
    sysConfig.batteryUnderVoltage = 42.0f;
    sysConfig.batteryOverCurrent = 25.0f;
    sysConfig.batteryOverTemp = 60.0f;
    sysConfig.solarVoltageLimit = 26.0f;
    sysConfig.fanTempOnThreshold = 35.0f;
    sysConfig.fanTempOffThreshold = 30.0f;
    sysConfig.chargingResumeSOC = 95.0f;
    
    // Save to NVS
    prefs.putFloat("ov_v", sysConfig.batteryOverVoltage);
    prefs.putFloat("un_v", sysConfig.batteryUnderVoltage);
    prefs.putFloat("ov_c", sysConfig.batteryOverCurrent);
    prefs.putFloat("ov_t", sysConfig.batteryOverTemp);
    prefs.putFloat("sol_v", sysConfig.solarVoltageLimit);
    prefs.putFloat("fan_on", sysConfig.fanTempOnThreshold);
    prefs.putFloat("fan_off", sysConfig.fanTempOffThreshold);
    prefs.putFloat("res_soc", sysConfig.chargingResumeSOC);
  } else {
    logInfo("CONFIG", "Loaded configurations from NVS.");
    sysConfig.batteryOverVoltage = prefs.getFloat("ov_v");
    sysConfig.batteryUnderVoltage = prefs.getFloat("un_v");
    sysConfig.batteryOverCurrent = prefs.getFloat("ov_c");
    sysConfig.batteryOverTemp = prefs.getFloat("ov_t");
    sysConfig.solarVoltageLimit = prefs.getFloat("sol_v");
    sysConfig.fanTempOnThreshold = prefs.getFloat("fan_on");
    sysConfig.fanTempOffThreshold = prefs.getFloat("fan_off");
    sysConfig.chargingResumeSOC = prefs.getFloat("res_soc");
  }
  
  prefs.end();
  printConfig();
}

SystemConfig getSystemConfig() {
  return sysConfig;
}

void setSystemConfig(SystemConfig cfg) {
  sysConfig = cfg;
  prefs.begin("syscfg", false);
  prefs.putFloat("ov_v", cfg.batteryOverVoltage);
  prefs.putFloat("un_v", cfg.batteryUnderVoltage);
  prefs.putFloat("ov_c", cfg.batteryOverCurrent);
  prefs.putFloat("ov_t", cfg.batteryOverTemp);
  prefs.putFloat("sol_v", cfg.solarVoltageLimit);
  prefs.putFloat("fan_on", cfg.fanTempOnThreshold);
  prefs.putFloat("fan_off", cfg.fanTempOffThreshold);
  prefs.putFloat("res_soc", cfg.chargingResumeSOC);
  prefs.end();
  logInfo("CONFIG", "Configuration updated and persisted in NVS.");
}

bool updateConfigParameter(uint8_t paramId, float value) {
  char dbg[64];
  snprintf(dbg, sizeof(dbg), "updateConfigParameter: id=%d, val=%.2f", paramId, value);
  logInfo("CONFIG", dbg);

  SystemConfig cfg = sysConfig;
  bool valid = true;
  switch (paramId) {
    case PARAM_BATTERY_OVER_VOLTAGE:
      if (value < 50.0f || value > 70.0f) valid = false;
      else cfg.batteryOverVoltage = value;
      break;
    case PARAM_BATTERY_UNDER_VOLTAGE:
      if (value < 35.0f || value > 55.0f || value >= (cfg.batteryOverVoltage - 2.0f)) valid = false;
      else cfg.batteryUnderVoltage = value;
      break;
    case PARAM_BATTERY_OVER_CURRENT:
      if (value < 1.0f || value > 35.0f) valid = false;
      else cfg.batteryOverCurrent = value;
      break;
    case PARAM_BATTERY_OVER_TEMP:
      if (value < 30.0f || value > 80.0f) valid = false;
      else cfg.batteryOverTemp = value;
      break;
    case PARAM_SOLAR_VOLTAGE_LIMIT:
      if (value < 15.0f || value > 50.0f) valid = false;
      else cfg.solarVoltageLimit = value;
      break;
    case PARAM_FAN_TEMP_ON:
      if (value < 20.0f || value > 60.0f) valid = false;
      else cfg.fanTempOnThreshold = value;
      break;
    case PARAM_FAN_TEMP_OFF:
      if (value < 15.0f || value > 55.0f || value >= cfg.fanTempOnThreshold) valid = false;
      else cfg.fanTempOffThreshold = value;
      break;
    case PARAM_CHARGING_RESUME_SOC:
      if (value < 10.0f || value > 98.0f) valid = false;
      else cfg.chargingResumeSOC = value;
      break;
    default:
      valid = false;
      break;
  }
  if (valid) {
    setSystemConfig(cfg);
    return true;
  }
  return false;
}

void printConfig() {
  char buf[256];
  snprintf(buf, sizeof(buf), 
    "CFG: OverV=%.1f UnderV=%.1f OverC=%.1f OverT=%.1f SolarV=%.1f FanON=%.1f FanOFF=%.1f ResumeSOC=%.1f",
    sysConfig.batteryOverVoltage, sysConfig.batteryUnderVoltage, sysConfig.batteryOverCurrent,
    sysConfig.batteryOverTemp, sysConfig.solarVoltageLimit, sysConfig.fanTempOnThreshold,
    sysConfig.fanTempOffThreshold, sysConfig.chargingResumeSOC);
  logInfo("CONFIG", buf);
}

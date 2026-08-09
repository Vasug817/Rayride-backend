#include "PowerOptimization.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "../debug/DebugLog.h"

static PowerMode currentMode = POWER_PERFORMANCE;
static bool initialized = false;

void initPowerOptimization() {
  logInfo("POWER", "Initializing Power Optimization...");
  // Enable Min Modem Sleep by default
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  currentMode = POWER_BALANCED;
  setCpuFrequencyMhz(160);
  initialized = true;
}

void setPowerMode(PowerMode mode) {
  if (currentMode == mode && initialized) return;

  currentMode = mode;
  switch (mode) {
    case POWER_PERFORMANCE:
      setCpuFrequencyMhz(240);
      esp_wifi_set_ps(WIFI_PS_NONE);
      logInfo("POWER", "Power Mode: PERFORMANCE (240MHz, Modem Sleep OFF)");
      break;
    case POWER_BALANCED:
      setCpuFrequencyMhz(160);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      logInfo("POWER", "Power Mode: BALANCED (160MHz, Min Modem Sleep)");
      break;
    case POWER_SAVING:
      setCpuFrequencyMhz(80);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      logInfo("POWER", "Power Mode: SAVING (80MHz, Min Modem Sleep)");
      break;
    case POWER_SLEEP:
      logInfo("POWER", "Power Mode: Entering Light Sleep for 1 second...");
      Serial.flush();
      
      // Configure wakeup timer for 1 second
      esp_sleep_enable_timer_wakeup(1000000); // 1 million microseconds
      
      // Enter light sleep
      esp_light_sleep_start();
      
      logInfo("POWER", "Woke up from Light Sleep");
      break;
  }
}

void optimizePowerState(ChargeState emsState, SolarData solar) {
  // If Wi-Fi is not connected, limit power to BALANCED to prevent current spikes
  // during connection attempts.
  extern bool isWiFiConnected();
  if (!isWiFiConnected()) {
    setPowerMode(POWER_BALANCED);
    return;
  }

  if (emsState == STATE_CHARGING) {
    if (solar.power >= 150.0) { // SOLAR_SUFFICIENT_W
      setPowerMode(POWER_PERFORMANCE);
    } else {
      setPowerMode(POWER_BALANCED);
    }
  } else if (emsState == STATE_FULLY_CHARGED) {
    setPowerMode(POWER_SAVING);
  } else if (emsState == STATE_IDLE || emsState == STATE_FAULT) {
    if (solar.voltage < 2.0) { // Solar voltage is negligible = night time!
      // In a real system, we could enter POWER_SLEEP here.
      // For desk-testing visibility, we stick to POWER_SAVING (80MHz) to keep serial print timing stable.
      setPowerMode(POWER_SAVING);
    } else {
      setPowerMode(POWER_SAVING);
    }
  }
}

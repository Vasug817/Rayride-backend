#include "WatchdogRecovery.h"
#include "config.h"
#include "../debug/DebugLog.h"
#include "../fault/FaultDetection.h"
#include <EEPROM.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

struct WatchdogState {
  uint32_t magic;
  uint8_t  consecutiveResets;
  uint8_t  lockedOut;
  uint16_t reserved;
  uint32_t checksum;
};

static bool lockedOut = false;

static uint32_t wdtStateChecksum(const WatchdogState &s) {
  return s.magic + s.consecutiveResets + s.lockedOut;
}

static void loadWatchdogState(WatchdogState &out) {
  EEPROM.get(WATCHDOG_STATE_EEPROM_OFFSET, out);
  if (out.magic != WATCHDOG_STATE_MAGIC || out.checksum != wdtStateChecksum(out)) {
    // Never written, or corrupted - start from a clean slate.
    out.magic = WATCHDOG_STATE_MAGIC;
    out.consecutiveResets = 0;
    out.lockedOut = 0;
    out.reserved = 0;
    out.checksum = wdtStateChecksum(out);
  }
}

static void saveWatchdogState(WatchdogState &s) {
  s.magic = WATCHDOG_STATE_MAGIC;
  s.checksum = wdtStateChecksum(s);
  EEPROM.put(WATCHDOG_STATE_EEPROM_OFFSET, s);
  EEPROM.commit();
}

static void configureTaskWatchdog() {
  // The esp_task_wdt API differs between arduino-esp32 core 2.x and 3.x
  // (this project has hit that exact core-version mismatch before, with
  // the ledcAttach/ledcSetup split) - handle both.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = (uint32_t)WATCHDOG_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdtConfig);
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);  // Register the loop() task
}

uint8_t initWatchdogRecovery() {
  EEPROM.begin(EEPROM_SIZE);  // Safe to call again even if already begun elsewhere

  esp_reset_reason_t reason = esp_reset_reason();
  bool wasWatchdogReset =
    (reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT || reason == ESP_RST_INT_WDT);

  WatchdogState state;
  loadWatchdogState(state);
  uint8_t reportedFault = FAULT_NONE;

  if (wasWatchdogReset) {
    state.consecutiveResets++;
    logWarn("WDT", "Booted after a watchdog reset");
    reportedFault = F012_WATCHDOG_RESET;

    if (state.consecutiveResets >= WATCHDOG_MAX_CONSECUTIVE_RESETS) {
      state.lockedOut = 1;
      logError("WDT", "Crash-loop detected - relay locked open");
      reportedFault = F013_WATCHDOG_LOCKOUT;
    }
    saveWatchdogState(state);
  } else if (state.lockedOut) {
    // A normal (non-WDT) reboot alone does NOT clear a prior lockout -
    // that requires an explicit clearWatchdogLockout() call.
    reportedFault = F013_WATCHDOG_LOCKOUT;
  }

  lockedOut = (state.lockedOut != 0);
  configureTaskWatchdog();
  return reportedFault;
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

void confirmWatchdogHealthy() {
  WatchdogState state;
  loadWatchdogState(state);
  if (state.consecutiveResets != 0) {
    state.consecutiveResets = 0;
    saveWatchdogState(state);  // Note: never clears lockedOut here
  }
}

bool isWatchdogLockedOut() {
  return lockedOut;
}

void clearWatchdogLockout() {
  WatchdogState state;
  loadWatchdogState(state);
  state.lockedOut = 0;
  state.consecutiveResets = 0;
  saveWatchdogState(state);
  lockedOut = false;
  logInfo("WDT", "Lockout cleared by operator");
}

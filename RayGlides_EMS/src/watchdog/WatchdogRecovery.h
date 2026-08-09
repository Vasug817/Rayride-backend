#ifndef WATCHDOG_RECOVERY_H
#define WATCHDOG_RECOVERY_H

#include <Arduino.h>

// Starts the ESP32 hardware task watchdog and inspects the previous
// boot's reset reason. If the last boot ended in a WDT reset, bumps a
// persisted consecutive-crash counter; at WATCHDOG_MAX_CONSECUTIVE_RESETS
// the system enters lockout (see isWatchdogLockedOut()).
// Call as early as possible in setup() - right after initRelay(), before
// anything (CAN/RS485/sensors) that could itself hang during init.
// Returns FAULT_NONE, F012_WATCHDOG_RESET, or F013_WATCHDOG_LOCKOUT
// (cast to FaultCode by the caller) so main.cpp can log/report it.
uint8_t initWatchdogRecovery();

// Feeds the hardware watchdog. Call exactly once, at the very end of
// each completed loop() iteration - never mid-loop, never from inside a
// step that might itself hang. That's what makes a real hang trigger a
// real reset instead of being fed through.
void feedWatchdog();

// Call once, WATCHDOG_HEALTHY_UPTIME_MS after boot, to clear the
// persisted crash counter now that the firmware has proven it's stable.
void confirmWatchdogHealthy();

// True once the system has entered crash-loop lockout. While true,
// RelayControl must force the relay open no matter what the charge
// state machine says.
bool isWatchdogLockedOut();

// Deliberate operator action only (serial command, service menu, etc.)
// - never called automatically. Clears the lockout and the counter.
void clearWatchdogLockout();

#endif

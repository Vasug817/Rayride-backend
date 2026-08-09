# Watchdog Recovery - Design Notes

## Problem
`loop()` can hang (blocked sensor read, a stuck CAN/RS485 wait, a bug).
Left alone, the ESP32 just sits there - the relay stays in whatever
position it was last commanded to, which may not be safe.

## Three layers

1. **Detect a hang** - the ESP32 hardware Task Watchdog Timer (TWDT) is
   fed once per completed `loop()` iteration (`feedWatchdog()`, called
   last, after `delay()` is the only thing left). If a cycle doesn't
   complete within `WATCHDOG_TIMEOUT_S`, the TWDT force-resets the chip.

2. **Recover safely** - `initRelay()` already defaults the relay to
   `RELAY_OPEN` on every boot, before anything else runs. This module
   piggybacks on that: it starts the TWDT as early as possible in
   `setup()` (right after `initRelay()`), and on boot it checks
   `esp_reset_reason()` to see if the *previous* boot ended in a WDT
   reset. If so, it reports `F012_WATCHDOG_RESET` so it gets logged
   like any other fault - a hang-and-recover is a real event worth
   knowing about, not something to hide.

3. **Crash-loop lockout** - one recovered hang is fine. Repeated hangs
   mean something is structurally broken, and letting the system keep
   rebooting into a state where it might close the relay again is
   dangerous. A consecutive-reset counter is persisted to EEPROM
   (separate offset from `PersistedEstimatorState`, its own magic +
   checksum). At `WATCHDOG_MAX_CONSECUTIVE_RESETS` it flips a persisted
   `lockedOut` flag and reports `F013_WATCHDOG_LOCKOUT` (CRITICAL).
   `RelayControl::updateRelay()` checks `isWatchdogLockedOut()` as a
   hard interlock, ahead of the normal critical-fault check.

   The counter only clears once `main.cpp` confirms
   `WATCHDOG_HEALTHY_UPTIME_MS` of stable uptime
   (`confirmWatchdogHealthy()`). The lockout flag itself is never
   cleared automatically by a reboot - only `clearWatchdogLockout()`,
   which is meant to be wired to a deliberate operator action.

## Why a counter instead of a time window
There's no battery-backed RTC here, so `millis()` resets to 0 every
boot - "N resets within 60 seconds" can't be measured reliably across
reboots without extra hardware. A consecutive-crash counter, cleared
only after proven healthy uptime, is the standard embedded pattern for
this and needs no wall-clock time.

## Known follow-up
`clearWatchdogLockout()` exists but nothing calls it yet - intentional.
Wiring it to something (serial command, long button-press at boot,
protocol message) is a deliberate next decision, not an oversight.

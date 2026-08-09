# Pseudocode — UART Debugging

Corresponds to: `DebugLog.h/.cpp`

Replaces scattered, unstructured `Serial.print()` calls with a single
consistent logging format, a runtime on/off switch, and level-based
filtering - all still transmitted over the same UART0/USB Serial link
already used for the console, but now as structured, parseable lines.

```
MODULE UARTDebugging

CONSTANTS
    LOG_INFO, LOG_WARN, LOG_ERROR      // increasing severity

STATE
    debugEnabled = true
    minLevel = LOG_INFO


FUNCTION InitDebugLog():
    debugEnabled = true
    minLevel = LOG_INFO
    Print("[DEBUG] UART debug logging initialized")


FUNCTION SetDebugEnabled(enabled):
    debugEnabled = enabled


FUNCTION SetMinLogLevel(level):
    minLevel = level


FUNCTION LogMessage(level, module, message):
    IF NOT debugEnabled:
        RETURN
    IF level < minLevel:
        RETURN                          // filtered out by current minimum

    timestamp = CurrentMillis()
    Print("[" + timestamp + "ms][" + LevelTag(level) + "][" + module + "] " + message)


FUNCTION LogInfo(module, message):   LogMessage(LOG_INFO, module, message)
FUNCTION LogWarn(module, message):   LogMessage(LOG_WARN, module, message)
FUNCTION LogError(module, message):  LogMessage(LOG_ERROR, module, message)
```

## Why structure it this way

- **Consistent format** (`[timestamp][LEVEL][MODULE] message`) makes the
  log parseable by a script or log viewer later, not just human-readable
  in a live terminal.
- **Runtime enable/disable** means a noisy or verbose subsystem can be
  quieted (or the whole log silenced) without recompiling - useful once
  the firmware moves from active development toward production, or for
  toggling verbosity remotely via a future COMMAND message over the
  existing RayGlidesProtocol/CAN/RS485 channels.
- **Level filtering** (`SetMinLogLevel`) lets WARN/ERROR stay visible
  even when INFO-level noise is turned off, so real problems don't get
  lost in routine telemetry.

## EMS integration

`main.cpp` calls `InitDebugLog()` once in `setup()`. Every fault
transition, state transition, and incoming CAN/RS485 message that used
to be a raw `Serial.print` is now a `logInfo`/`logWarn`/`logError` call
tagged with its owning module ("BATTERY", "FAULT", "CAN", "RS485"), and
the per-cycle telemetry summary is logged as a single structured
"TELEMETRY" line each loop instead of several unlabeled print statements.

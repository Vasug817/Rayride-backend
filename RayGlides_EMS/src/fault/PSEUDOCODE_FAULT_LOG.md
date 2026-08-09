# Pseudocode — Fault Logging System

Corresponds to: `FaultLog.h/.cpp`

Two-tier design: a fast in-RAM ring buffer holds ALL fault events
(Warning and Critical) for live diagnostics, while a smaller persistent
store (ESP32 NVS/flash) retains only CRITICAL faults across power
cycles/resets - similar in spirit to how automotive DTC (Diagnostic
Trouble Code) logs work: routine warnings are transient, safety-relevant
faults are retained.

```
MODULE FaultLoggingSystem

CONSTANTS
    RAM_LOG_MAX = 20          // all-severity ring buffer size
    PERSIST_MAX = 10           // NVS-persisted ring size (critical only)

STATE
    ramBuffer[RAM_LOG_MAX]     // ring buffer of (timestamp, code, severity)
    ramHead = 0
    ramCount = 0


FUNCTION InitFaultLog():
    ramHead = 0
    ramCount = 0

    OpenPersistentStorage(readonly=true)
    persistedCount = ReadInt("pcount", default=0)
    persistedHead = ReadInt("phead", default=0)

    // Replay persisted critical history back into RAM, oldest first,
    // so the log has continuity immediately after a reboot
    FOR i FROM 0 TO persistedCount - 1:
        slot = (persistedHead - persistedCount + i) MOD PERSIST_MAX
        entry = ReadBytes("pe" + slot)
        IF entry is valid:
            PushToRam(entry)
    ClosePersistentStorage()

    LogInfo("FAULTLOG", "Initialized, " + persistedCount + " persisted entries loaded")


FUNCTION LogFaultEvent(code, severity):
    entry = (timestamp=Now(), code=code, severity=severity)
    PushToRam(entry)

    IF severity != CRITICAL:
        RETURN                          // Warnings stay RAM-only, no flash wear

    OpenPersistentStorage(readonly=false)
    persistedCount = ReadInt("pcount", default=0)
    persistedHead = ReadInt("phead", default=0)

    WriteBytes("pe" + persistedHead, entry)
    persistedHead = (persistedHead + 1) MOD PERSIST_MAX
    IF persistedCount < PERSIST_MAX:
        persistedCount += 1

    WriteInt("pcount", persistedCount)
    WriteInt("phead", persistedHead)
    ClosePersistentStorage()


FUNCTION PushToRam(entry):
    ramBuffer[ramHead] = entry
    ramHead = (ramHead + 1) MOD RAM_LOG_MAX
    IF ramCount < RAM_LOG_MAX:
        ramCount += 1


FUNCTION GetFaultLogEntry(indexFromNewest):
    // 0 = most recent, 1 = next most recent, etc.
    idx = (ramHead - 1 - indexFromNewest) MOD RAM_LOG_MAX
    RETURN ramBuffer[idx]


FUNCTION ClearFaultLog():
    ramHead = 0
    ramCount = 0
    ErasePersistentStorage()


FUNCTION PrintFaultLog():
    LogInfo("FAULTLOG", "--- Fault Log (" + ramCount + " entries) ---")
    FOR i FROM 0 TO ramCount - 1:
        entry = GetFaultLogEntry(i)
        LogInfo("FAULTLOG", "[" + entry.timestamp + "ms] " + FaultName(entry.code) +
                             " (severity=" + entry.severity + ")")
```

## Why not persist everything?

Flash memory (NVS) has a limited write-cycle lifespan. Warning-level
faults (like a momentary under-voltage reading) can happen often during
normal operation and don't need to survive a reboot to be useful - the
RAM ring buffer already covers "what's happened recently" for live
diagnostics. Critical faults (battery not detected, over-voltage,
over-current, over-temperature) are rare, safety-relevant, and exactly
the kind of history worth keeping even if the device loses power or
resets right after - so only those get written to flash.

## EMS integration

`main.cpp` calls `InitFaultLog()` once in `setup()` (which also replays
any persisted critical history) and immediately calls `PrintFaultLog()`
so the console shows continuity across a reboot. Every time a new fault
latches in the main loop - exactly the same point where it's already
reported over USB Serial, CAN, and RS485 - `LogFaultEvent()` is called
too, so the fault log, the live comm buses, and the debug console all
stay in sync from a single point in the code.

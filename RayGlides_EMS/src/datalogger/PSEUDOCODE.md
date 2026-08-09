# Pseudocode — Data Logger

Corresponds to: `DataLogger.h/.cpp`

Distinct from `FaultLog` (which only records fault events): this logs a
periodic snapshot of the *entire* system state - battery, solar, charge
state, charging mode, active fault - for later analysis of normal
operation, not just failures. It also owns the EEPROM checkpointing of
estimator state, tying `EEPROMStorage` and the SOC/SOH estimators
together into one coherent "remember things across a reboot" system.

```
MODULE DataLogger

CONSTANTS
    MAX_RECORDS = 30
    RECORD_INTERVAL_MS = 10000       // snapshot to RAM every 10s
    CHECKPOINT_INTERVAL_MS = 60000    // persist estimator state every 60s

STATE
    ramBuffer[MAX_RECORDS]
    ramHead = 0
    ramCount = 0
    lastRecordMillis = 0
    lastCheckpointMillis = 0


FUNCTION InitDataLogger():
    ramHead = 0
    ramCount = 0
    lastCheckpointMillis = Now()

    InitEEPROMStorage()

    result = LoadPersistedState()
    IF result.valid:
        RestoreSOCEstimatorState(result.state.coulombSOC)
        RestoreSOHEstimatorState(result.state.sohResistance,
                                  result.state.sohCycleCount,
                                  result.state.sohThroughputAh)
        LogInfo("DATALOGGER", "Restored from EEPROM: SOC=" + result.state.coulombSOC + "%")
    ELSE:
        LogInfo("DATALOGGER", "No valid EEPROM checkpoint found - starting fresh")


FUNCTION UpdateDataLogger(battery, solar, state, mode, fault):
    now = Now()

    IF (now - lastRecordMillis) >= RECORD_INTERVAL_MS:
        RecordSnapshot(battery, solar, state, mode, fault)
        lastRecordMillis = now

    IF (now - lastCheckpointMillis) >= CHECKPOINT_INTERVAL_MS:
        CheckpointToEEPROM()
        lastCheckpointMillis = now


FUNCTION RecordSnapshot(battery, solar, state, mode, fault):
    record = (
        timestamp = Now(),
        battVoltage = battery.voltage, battCurrent = battery.current,
        battTemp = battery.temperature, battSOC = battery.soc, battSOH = battery.soh,
        solarVoltage = solar.voltage, solarPower = solar.power,
        state = state, mode = mode, fault = fault
    )
    ramBuffer[ramHead] = record
    ramHead = (ramHead + 1) MOD MAX_RECORDS
    IF ramCount < MAX_RECORDS:
        ramCount += 1


FUNCTION CheckpointToEEPROM():
    state.coulombSOC = GetSOCEstimatorState()
    (state.sohResistance, state.sohCycleCount, state.sohThroughputAh) = GetSOHEstimatorState()
    SavePersistedState(state)
    LogInfo("DATALOGGER", "Estimator state checkpointed to EEPROM")


FUNCTION PrintDataLog():
    LogInfo("DATALOGGER", "--- Data Log (" + ramCount + " records) ---")
    FOR i FROM 0 TO ramCount - 1:
        record = GetLogRecord(indexFromNewest=i)
        LogInfo("DATALOGGER", FormatRecord(record))
```

## Why two different intervals (10s records vs. 60s checkpoints)

The RAM history buffer is cheap to write to (just RAM), so it can afford
to sample fairly often (every 10s) to give a reasonably detailed recent
history for `printDataLog()` to show - 30 records at 10s each covers 5
minutes of history. The EEPROM checkpoint is a real flash write with a
limited write-cycle lifespan, so it's deliberately much less frequent
(every 60s) - frequent enough that a reboot loses at most a minute of
estimator drift, infrequent enough not to wear the flash under normal
operation.

## EMS integration

`main.cpp` calls `InitDataLogger()` once in `setup()`, positioned so
that any restored SOC/SOH state is in place *before* the main loop's
first sensor read - the estimators then continue from the restored
values rather than their normal fresh-start behavior. `UpdateDataLogger()`
is called every loop cycle with the current battery/solar/state/mode/
fault snapshot; its internal timers handle the actual throttling, so the
caller doesn't need to track intervals itself.

# Pseudocode — EEPROM Storage

Corresponds to: `EEPROMStorage.h/.cpp`

Uses the ESP32's EEPROM emulation library (a small region of flash
presented as byte-addressable storage). Persists the SOC/SOH estimators'
internal state, since without this they silently reset to default
assumptions ("50% charged, brand new battery") on every reboot.

```
MODULE EEPROMStorage

CONSTANTS
    EEPROM_SIZE = 64 bytes
    STATE_MAGIC = 0xEA57C0DE     // marks the region as containing valid data

STRUCT PersistedEstimatorState
    magic            // uint32
    coulombSOC       // float - from SOCEstimator
    sohResistance    // float - from SOHEstimator
    sohCycleCount    // int32 - from SOHEstimator
    sohThroughputAh  // float - from SOHEstimator (partial-cycle progress)
    checksum         // uint32


FUNCTION InitEEPROMStorage():
    OpenEEPROMRegion(EEPROM_SIZE)


FUNCTION ComputeChecksum(state):
    sum = state.magic
    sum += BitPattern(state.coulombSOC)
    sum += BitPattern(state.sohResistance)
    sum += state.sohCycleCount
    sum += BitPattern(state.sohThroughputAh)
    RETURN sum


FUNCTION LoadPersistedState():
    loaded = ReadStructFromEEPROM(offset=0)

    IF loaded.magic != STATE_MAGIC:
        RETURN (valid=false)          // never written, or wrong firmware version

    IF loaded.checksum != ComputeChecksum(loaded):
        RETURN (valid=false)          // corrupted (e.g. power lost mid-write)

    RETURN (valid=true, state=loaded)


FUNCTION SavePersistedState(state):
    state.magic = STATE_MAGIC
    state.checksum = ComputeChecksum(state)
    WriteStructToEEPROM(offset=0, state)
    CommitToFlash()                    // EEPROM emulation requires an explicit commit
```

## Why a magic number and checksum

The very first time the firmware ever runs, the EEPROM region contains
whatever was left over from the factory/previous firmware - essentially
random bytes. Reading that as if it were a valid `PersistedEstimatorState`
would feed garbage numbers straight into the SOC/SOH estimators. The
magic number lets `LoadPersistedState()` immediately recognize "this
region has never been written by this firmware" and fall back to fresh
defaults instead. The checksum catches the rarer case where a write was
interrupted partway (e.g. power loss during `SavePersistedState()`),
leaving a region with a valid-looking magic number but corrupted data.

## EMS integration

`DataLogger.initDataLogger()` calls `InitEEPROMStorage()` once, then
`LoadPersistedState()` - if valid, the loaded values are fed into
`SOCEstimator.restoreSOCEstimatorState()` and
`SOHEstimator.restoreSOHEstimatorState()` before the main loop ever runs,
so estimation continues smoothly across a reboot instead of resetting.

# Pseudocode — Battery SOH (State of Health) Estimation

Corresponds to: `SOHEstimator.h/.cpp`, called from `BatteryMonitor.cpp`.

SOH is distinct from SOC: SOC is how full the battery is *right now*; SOH
is how degraded the battery is over its *lifetime*. A fresh pack and a
heavily-cycled pack can both read 80% SOC at some moment, while having
very different SOH.

```
MODULE SOHEstimation

CONSTANTS
    NOMINAL_INTERNAL_RESISTANCE = 0.10   // ohms, "new battery" baseline
    MIN_DELTA_I_FOR_R_ESTIMATE = 1.0     // amps
    MAX_PLAUSIBLE_RESISTANCE = 5.0       // ohms - reject noisy outliers above this
    R_EMA_ALPHA = 0.9                     // smoothing weight
    DEGRADATION_PER_CYCLE_PCT = 0.02      // % health lost per equivalent full cycle
    BATTERY_CAPACITY_AH = 20.0

STATE
    lastVoltage = 0
    lastCurrent = 0
    runningResistance = -1        // sentinel: no estimate yet
    cumulativeThroughputAh = 0
    cycleCount = 0
    initialized = false


// --- Signal 1: opportunistic internal resistance estimation ---
// Whenever current changes enough between two samples, dV/dI gives a
// rough resistance reading. Averaged over time (EMA) to smooth noise.

FUNCTION UpdateInternalResistanceEstimate(voltage, current):
    dV = voltage - lastVoltage
    dI = current - lastCurrent

    IF ABS(dI) >= MIN_DELTA_I_FOR_R_ESTIMATE:
        rSample = ABS(dV / dI)
        IF rSample <= MAX_PLAUSIBLE_RESISTANCE:
            IF runningResistance < 0:
                runningResistance = rSample
            ELSE:
                runningResistance = (R_EMA_ALPHA * runningResistance) +
                                     ((1 - R_EMA_ALPHA) * rSample)


// --- Signal 2: equivalent full-cycle counting ---
// Total Coulomb throughput (both charge and discharge count toward
// cycling stress), converted into "equivalent full cycles."

FUNCTION UpdateCycleCount(current, dtSeconds):
    cumulativeThroughputAh += ABS(current) * (dtSeconds / 3600)
    WHILE cumulativeThroughputAh >= BATTERY_CAPACITY_AH:
        cumulativeThroughputAh -= BATTERY_CAPACITY_AH
        cycleCount += 1


// --- Combine into one SOH estimate ---

FUNCTION EstimateSOH(voltage, current):
    IF NOT initialized:
        lastVoltage = voltage
        lastCurrent = current
        initialized = true
        RETURN 100                 // no history yet - assume full health

    dtSeconds = TimeSinceLastCall()

    UpdateInternalResistanceEstimate(voltage, current)
    UpdateCycleCount(current, dtSeconds)

    lastVoltage = voltage
    lastCurrent = current

    sohResistance = 100
    IF runningResistance > 0:
        sohResistance = Clamp((NOMINAL_INTERNAL_RESISTANCE / runningResistance) * 100, 0, 100)

    sohCycles = Clamp(100 - (cycleCount * DEGRADATION_PER_CYCLE_PCT), 0, 100)

    RETURN Min(sohResistance, sohCycles)   // health limited by the worse indicator
```

## Why two signals, combined with Min()

Internal resistance rising is one of the clearest fingerprints of battery
aging (electrolyte breakdown, electrode degradation), and can be
estimated opportunistically without a dedicated load test. Cycle-count
fade is the other standard degradation model (every charge/discharge
cycle chemically wears the cells a little). Neither alone is a complete
picture - a pack could have low cycle count but already show resistance
growth from calendar aging or heat exposure, or vice versa - so health is
reported as whichever indicator is currently worse, a conservative choice
consistent with how automotive BMS systems typically report SOH.

## EMS integration

`BatteryMonitor.cpp`'s `readBatteryData()` calls `estimateSOH()` and
stores the result in `BatteryData.soh`, alongside the SOC estimate. This
value now flows into every telemetry channel the rest of the system
already had (USB Serial protocol, CAN, RS485 - all three status frames
carry SOH as their 8th payload byte, alongside SOC/state/voltage/current/
temperature/solar readings).

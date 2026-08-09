# Pseudocode — Battery SOC Estimation

Corresponds to: `SOCEstimator.h/.cpp`, called from `BatteryMonitor.cpp`.

Replaces the previous naive calculation (`soc = rawADC / 4095 * 100`,
which is really just relabeling the ADC reading) with two real estimation
methods, combined into one hybrid approach.

```
MODULE SOCEstimation

CONSTANTS
    OCV_TABLE = [                    // (voltage, SOC%) pairs, increasing
        (42.0,   0.0), (46.0,   5.0), (48.0,  10.0), (50.0,  20.0),
        (52.0,  35.0), (54.0,  50.0), (56.0,  65.0), (58.0,  78.0),
        (60.0,  88.0), (62.0,  94.0), (64.0,  97.0), (66.0,  99.0),
        (68.0, 100.0)
    ]
    BATTERY_CAPACITY_AH = 20.0
    REST_CURRENT_THRESHOLD = 0.5      // amps
    BLEND_ALPHA = 0.9                  // weight kept on Coulomb count

STATE
    coulombSOC = -1                    // sentinel: not yet initialized
    lastUpdateMillis = 0
    initialized = false


// --- Method 1: Voltage-based (OCV lookup table) ---
// Accurate at rest; under load, IR drop makes it pessimistic while
// discharging and optimistic while charging.

FUNCTION EstimateSOC_OCV(voltage):
    IF voltage <= OCV_TABLE[first].voltage:  RETURN OCV_TABLE[first].soc
    IF voltage >= OCV_TABLE[last].voltage:   RETURN OCV_TABLE[last].soc

    FOR EACH consecutive pair (low, high) IN OCV_TABLE:
        IF voltage BETWEEN low.voltage AND high.voltage:
            fraction = (voltage - low.voltage) / (high.voltage - low.voltage)
            RETURN low.soc + fraction * (high.soc - low.soc)     // linear interpolation


// --- Method 2: Coulomb counting (current integration) ---
// Tracks SOC continuously regardless of voltage sag, but drifts over
// time as small sensor offset/noise accumulates.

FUNCTION EstimateSOC_CoulombCounting(current, dtSeconds):
    IF coulombSOC < 0:
        coulombSOC = 50.0             // fallback starting point if used alone

    deltaAh = current * (dtSeconds / 3600)
    coulombSOC = coulombSOC + (deltaAh / BATTERY_CAPACITY_AH) * 100
    coulombSOC = Clamp(coulombSOC, 0, 100)
    RETURN coulombSOC


// --- Hybrid: Coulomb counting for continuous tracking, corrected
// toward the OCV estimate whenever the battery is close to at-rest ---

FUNCTION EstimateSOC_Hybrid(voltage, current):
    now = CurrentMillis()

    IF NOT initialized:
        coulombSOC = EstimateSOC_OCV(voltage)   // no history yet - anchor to voltage
        lastUpdateMillis = now
        initialized = true
        RETURN coulombSOC

    dtSeconds = (now - lastUpdateMillis) / 1000
    lastUpdateMillis = now

    EstimateSOC_CoulombCounting(current, dtSeconds)   // updates coulombSOC in place

    IF ABS(current) <= REST_CURRENT_THRESHOLD:
        // Voltage reading is trustworthy right now - correct any drift
        ocvEstimate = EstimateSOC_OCV(voltage)
        coulombSOC = (BLEND_ALPHA * coulombSOC) + ((1 - BLEND_ALPHA) * ocvEstimate)

    coulombSOC = Clamp(coulombSOC, 0, 100)
    RETURN coulombSOC
```

## Why hybrid, not just one method

Voltage alone is only accurate when the battery is at rest - under load,
internal resistance causes the terminal voltage to sag below the true
open-circuit voltage, so a purely voltage-based estimate would swing
around every time current changes even if the actual charge level barely
moved. Coulomb counting alone is smooth and load-independent, but has no
way to correct itself - small calibration errors in the current sensor
accumulate indefinitely. Blending the two (Coulomb counting for continuous
tracking, periodically re-anchored to the OCV table whenever current is
near zero) is a standard practical compromise used in real battery
management systems.

## EMS integration

`BatteryMonitor.cpp`'s `readBatteryData()` calls `estimateSOC_Hybrid()`
directly (replacing the old raw-ADC-percentage line), so `BatteryData.soc`
is now a genuine estimate rather than a relabeled sensor reading - every
downstream consumer (the battery state machine, the RayGlidesProtocol/CAN/
RS485 status frames) picks up the improved value automatically without
any other module needing to change.

# Pseudocode — MPPT (Perturb & Observe, Incremental Conductance)

Corresponds to: `MPPTAlgorithm.h/.cpp`

Both algorithms share the same job: adjust a PWM duty cycle commanding a
DC-DC converter so the solar panel operates at (or near) its maximum
power point, using only the voltage and current readings from
`SolarMonitor`.

```
MODULE MPPT

CONSTANTS
    DUTY_STEP = 0.01          // 1% per perturbation
    DUTY_MIN = 0.0
    DUTY_MAX = 0.95
    POWER_EPSILON = 0.05      // watts
    VOLTAGE_EPSILON = 0.02    // volts
    CURRENT_EPSILON = 0.01    // amps
    COND_EPSILON = 0.01

STATE
    prevVoltage = 0
    prevCurrent = 0
    prevPower = 0
    dutyCycle = 0.5           // starting point


FUNCTION InitMPPT(startingDuty):
    dutyCycle = Clamp(startingDuty, DUTY_MIN, DUTY_MAX)
    prevVoltage = 0
    prevCurrent = 0
    prevPower = 0


// --- Perturb & Observe ---
// Idea: nudge the duty cycle a little, see if power went up or down,
// and keep going the same way if it helped - reverse if it didn't.

FUNCTION PerturbAndObserve(solarData):
    power = solarData.power
    deltaP = power - prevPower
    deltaV = solarData.voltage - prevVoltage

    IF ABS(deltaP) > POWER_EPSILON:
        IF deltaP > 0:
            // Power improved - keep perturbing the same direction as last time
            IF deltaV > 0:  dutyCycle = dutyCycle + DUTY_STEP
            ELSE:            dutyCycle = dutyCycle - DUTY_STEP
        ELSE:
            // Power got worse - last perturbation was the wrong way, reverse
            IF deltaV > 0:  dutyCycle = dutyCycle - DUTY_STEP
            ELSE:            dutyCycle = dutyCycle + DUTY_STEP
    // ELSE: power essentially unchanged - close enough to the MPP, hold

    dutyCycle = Clamp(dutyCycle, DUTY_MIN, DUTY_MAX)

    prevVoltage = solarData.voltage
    prevCurrent = solarData.current
    prevPower = power

    RETURN dutyCycle


// --- Incremental Conductance ---
// Idea: at the true maximum power point, dP/dV = 0, which expands to
// I + V*(dI/dV) = 0, i.e. dI/dV = -I/V. Compare the incremental
// conductance (dI/dV) against the instantaneous conductance (-I/V)
// directly, rather than only inferring direction from trial and error.

FUNCTION IncrementalConductance(solarData):
    deltaV = solarData.voltage - prevVoltage
    deltaI = solarData.current - prevCurrent

    IF ABS(deltaV) < VOLTAGE_EPSILON:
        // No meaningful voltage change since last cycle
        IF ABS(deltaI) < CURRENT_EPSILON:
            // dI ~ 0 too - already at the MPP, hold
        ELSE IF deltaI > 0:
            dutyCycle = dutyCycle + DUTY_STEP     // Irradiance rising - push Vref up
        ELSE:
            dutyCycle = dutyCycle - DUTY_STEP     // Irradiance falling - pull Vref down
    ELSE:
        instantaneousConductance = solarData.current / solarData.voltage    // I / V
        incrementalConductance = deltaI / deltaV                             // dI / dV

        IF ABS(incrementalConductance + instantaneousConductance) < COND_EPSILON:
            // dI/dV == -I/V (within tolerance) - sitting right at the MPP, hold
        ELSE IF incrementalConductance > -instantaneousConductance:
            dutyCycle = dutyCycle + DUTY_STEP     // Left of the MPP - move right
        ELSE:
            dutyCycle = dutyCycle - DUTY_STEP     // Right of the MPP - move left

    dutyCycle = Clamp(dutyCycle, DUTY_MIN, DUTY_MAX)

    prevVoltage = solarData.voltage
    prevCurrent = solarData.current
    prevPower = solarData.power

    RETURN dutyCycle


FUNCTION ApplyDutyCycle(duty):
    pwmValue = duty * MAX_PWM_COUNT
    WritePWM(MPPT_PWM_PIN, pwmValue)
```

## Why two algorithms

**Perturb & Observe** is simpler and cheaper to compute, but has a known
weakness: it can lose track of the MPP if irradiance changes quickly
(e.g. a cloud passing), because it only compares this cycle's power to
last cycle's - a sudden environmental change looks the same as its own
perturbation.

**Incremental Conductance** uses the actual power-vs-voltage derivative
relationship, so it can detect the true MPP condition directly rather
than inferring it from trial and error, and tracks changing irradiance
more accurately - at the cost of being slightly more sensitive to noisy
sensor readings (division by a small `deltaV` needs the epsilon guard
above).

## EMS integration

`main.cpp` reads `SolarData` once per cycle (from `SolarMonitor`) and
passes the raw sample, plus the cycle's `activeFault`, into a single
orchestrator, `updateMPPT()`. Its return value is applied to the PWM pin
internally (never through a separate `applyDutyCycle()` call from
`main.cpp`) and printed to Serial each cycle, so tracking behavior is
visible alongside the rest of the system's telemetry.

```
MODULE MPPT (extended: PWM lifecycle + protection)

CONSTANTS (additional, see config.h)
    MPPT_INTERVAL_MS = 200               // algorithm re-runs this often, not every loop() spin
    MPPT_SOFT_START_DURATION_MS = 3000   // 0% -> target duty ramp time
    MPPT_DUTY_SLEW_RATE_PER_S = 0.5      // max duty fraction change per second, always enforced
    MPPT_FILTER_ALPHA = 0.2              // EMA weight on the newest V/I sample

STATE (additional)
    commandedDuty = 0        // last duty actually written to hardware (post-slew)
    pwmEnabled = false
    wasShutdownByFault = false
    softStartActive, softStartBeginMs, softStartFromDuty, softStartTargetDuty
    lastSlewUpdateMs, lastMPPTRunMs, mpptHasRun
    filtVoltage, filtCurrent, filterInitialized


FUNCTION IsPWMShutdownFault(fault):
    RETURN fault IN { NOT_DETECTED, OVER_VOLTAGE, UNDER_VOLTAGE,
                       OVER_TEMPERATURE, COMM_TIMEOUT, OVER_CURRENT }


FUNCTION EnablePWM():                       // and the internal BeginSoftStart(now) it wraps
    softStartFromDuty = commandedDuty       // usually 0, coming from disabled
    softStartTargetDuty = dutyCycle         // ramp toward the algorithm's current target
    softStartBeginMs = now
    softStartActive = true
    pwmEnabled = true
    lastSlewUpdateMs = now
    ResetSolarFilter()                      // don't blend stale pre-enable readings in


FUNCTION DisablePWM():
    pwmEnabled = false
    softStartActive = false
    commandedDuty = 0
    WritePWM(MPPT_PWM_PIN, 0)


FUNCTION FilterSolarData(raw):
    IF NOT filterInitialized:
        filtVoltage, filtCurrent = raw.voltage, raw.current
        filterInitialized = true
    ELSE:
        filtVoltage += ALPHA * (raw.voltage - filtVoltage)   // EMA low-pass
        filtCurrent += ALPHA * (raw.current - filtCurrent)
    RETURN SolarData{ voltage: filtVoltage, current: filtCurrent,
                       power: filtVoltage * filtCurrent }


FUNCTION UpdateMPPT(method, rawSolar, activeFault, now):
    IF IsPWMShutdownFault(activeFault):
        IF pwmEnabled: DisablePWM()
        wasShutdownByFault = true
        RETURN { duty: 0, active: false }

    IF wasShutdownByFault AND NOT pwmEnabled:
        BeginSoftStart(now)                 // fault cleared - resume automatically, softly
        wasShutdownByFault = false

    IF NOT pwmEnabled:
        RETURN { duty: 0, active: false }   // deliberately disabled, stays off

    filtered = FilterSolarData(rawSolar)

    IF softStartActive:
        elapsed = now - softStartBeginMs
        IF elapsed >= MPPT_SOFT_START_DURATION_MS:
            target = softStartTargetDuty
            softStartActive = false
            // hand off to normal tracking from here, not from stale history
            dutyCycle = target
            prevVoltage, prevCurrent, prevPower = filtered.voltage, filtered.current, filtered.power
        ELSE:
            progress = elapsed / MPPT_SOFT_START_DURATION_MS
            target = softStartFromDuty + progress * (softStartTargetDuty - softStartFromDuty)
    ELSE:
        IF NOT mpptHasRun OR (now - lastMPPTRunMs) >= MPPT_INTERVAL_MS:
            RunSelectedAlgorithm(method, filtered)   // updates dutyCycle in place
            lastMPPTRunMs = now
            mpptHasRun = true
        target = dutyCycle

    dt = (now - lastSlewUpdateMs) / 1000
    lastSlewUpdateMs = now
    maxDelta = MPPT_DUTY_SLEW_RATE_PER_S * dt
    delta = Clamp(target - commandedDuty, -maxDelta, maxDelta)
    ApplyDutyCycle(commandedDuty + delta)   // writes hardware PWM, updates commandedDuty

    RETURN { duty: commandedDuty, voltageRef: filtered.voltage, active: true }
```

### Why each piece

- **Soft-start** avoids slamming the DC-DC converter with a large duty
  step the instant it's enabled (at boot, or coming back from a fault),
  which would otherwise show up as an inrush current spike.
- **PWM enable/disable** gives explicit, named control over the
  converter's on/off state instead of only ever writing a duty value.
- **Automatic fault shutdown** (`isPWMShutdownFault`) treats overcurrent,
  overvoltage, undervoltage, overtemperature, battery-not-detected, and
  communication timeout as "stop converting now" conditions - the
  converter shouldn't keep pushing power while any of these are active.
  Recovery is automatic once the fault clears, but always re-enters
  through a fresh soft start rather than resuming at the pre-fault duty.
- **Fixed-interval MPPT** decouples the perturbation/decision cadence
  from `loop()`'s own timing, so the tracking behavior stays consistent
  even if the surrounding loop speeds up or slows down.
- **Slew-rate limiting** is enforced on every single write to the PWM
  peripheral - including during soft start and immediately after a fault
  clears - so the converter's output can never jump faster than
  `MPPT_DUTY_SLEW_RATE_PER_S` allows, regardless of what the algorithm or
  soft-start ramp asks for.
- **Sensor filtering** (EMA low-pass on solar voltage/current) keeps ADC
  noise from being misread as a real power change, which both P&O and
  Incremental Conductance are sensitive to.

## Configurable PWM frequency

`initMPPT()` brings the LEDC timer up at the default `MPPT_PWM_FREQ_HZ`.
`setPWMFrequency(freqHz)` retunes it at runtime (e.g. to trade switching
losses vs. inductor/capacitor sizing for a different converter build)
without disturbing whatever duty cycle is currently commanded -
`getPWMFrequency()` reads back the value in effect.

## Adaptive step size

Both algorithms replaced their old fixed 1% perturbation with
`adaptiveStepSize(deltaP)`:

```
FUNCTION AdaptiveStepSize(deltaP):
    ratio = Clamp(ABS(deltaP) / MPPT_ADAPTIVE_POWER_SCALE, 0, 1)
    RETURN MPPT_DUTY_STEP_MIN + ratio * (MPPT_DUTY_STEP_MAX - MPPT_DUTY_STEP_MIN)
```

A big swing in power (a cloud edge, a sudden load change) gets a big
step so the tracker catches up quickly; a near-zero swing - meaning it's
already sitting close to the MPP - gets the smallest step, so it settles
quietly instead of dithering. Incremental Conductance reuses the same
function (computed from its own `deltaP`) so both algorithms scale
consistently.

## Runtime diagnostics

`getMPPTDiagnostics(now)` returns a point-in-time `MPPTDiagnostics`
snapshot - commanded duty, PWM frequency, enabled/active/soft-start
status, active algorithm, the fault currently forcing a shutdown (if
any), filtered V/I/P, the last adaptive step size used, whether the most
recent write was slew-limited, and time since the last MPPT decision.
`printMPPTDiagnostics(now)` logs it as one line via `DebugLog`; `main.cpp`
calls this once per loop alongside the existing telemetry line.

## Performance statistics

`MPPTStats` accumulates simple cumulative counters - time enabled, number
of MPPT decisions, soft starts, fault-triggered shutdowns, slew-limited
writes, and running min/avg/max commanded duty. `updateMPPT()` updates
these every call and automatically logs a one-line summary via
`logMPPTStats()` every `MPPT_STATS_LOG_INTERVAL_MS` (30s by default) -
no separate call needed from `main.cpp`. `resetMPPTStats()` clears the
counters (also called internally by `initMPPT()`).

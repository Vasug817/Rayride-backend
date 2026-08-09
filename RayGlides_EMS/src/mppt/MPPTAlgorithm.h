#ifndef MPPT_ALGORITHM_H
#define MPPT_ALGORITHM_H

#include <Arduino.h>
#include "../solar/SolarMonitor.h"
#include "../fault/FaultDetection.h"

enum MPPTMethod { MPPT_PERTURB_OBSERVE, MPPT_INCREMENTAL_CONDUCTANCE };

struct MPPTResult {
  float dutyCycle;    // 0.0-1.0, PWM duty cycle actually commanded to the DC-DC converter
  float voltageRef;   // Filtered panel voltage at the time of this decision (informational)
  bool  pwmActive;    // false when the converter is shut down (disabled or fault)
};

// Runtime snapshot of everything a diagnostics/telemetry consumer would
// want to know about the converter's current state, in one call.
struct MPPTDiagnostics {
  float    dutyCycle;             // Commanded duty actually on the PWM pin right now
  uint32_t pwmFrequencyHz;         // Current LEDC timer frequency
  bool     pwmEnabled;             // Deliberately enabled (independent of fault shutdown)
  bool     pwmActive;              // Enabled AND not currently fault-shutdown
  bool     softStartActive;
  float    softStartProgress;      // 0.0-1.0; only meaningful while softStartActive
  MPPTMethod method;                // Algorithm that produced the last decision
  FaultCode shutdownFault;          // FAULT_NONE unless a shutdown fault is currently active
  float    filteredVoltage;
  float    filteredCurrent;
  float    filteredPower;
  float    algorithmTargetDuty;    // Pre-slew target from the last MPPT decision (or soft-start step)
  float    lastStepSize;            // Adaptive perturbation size used on the last MPPT decision
  bool     slewLimited;             // True if the last updateMPPT() write was clamped by the slew limiter
  unsigned long msSinceLastMPPTRun; // Time since the algorithm last actually ran a decision
};

// Basic cumulative performance counters, reset with resetMPPTStats().
// Deliberately simple (no floating averages of averages, no persistence) -
// this is a field-diagnostics summary, not a data logger.
struct MPPTStats {
  unsigned long enabledTimeMs;       // Cumulative time PWM has been enabled & active
  unsigned long faultShutdownCount;   // Number of times a fault forced a shutdown
  unsigned long softStartCount;       // Number of soft-start ramps initiated (boot + recoveries)
  unsigned long mpptDecisionCount;    // Number of times the MPPT algorithm actually ran
  unsigned long slewLimitedCount;     // Number of updateMPPT() calls where the slew limiter clamped the step
  float          minDutyObserved;
  float          maxDutyObserved;
  float          avgDutyObserved;     // Running mean of the commanded duty, sampled once per updateMPPT() call
};

// --- Setup / lifecycle ---

// Resets internal tracking state (previous V/I/P), the sensor filter, the
// starting duty cycle target, diagnostics/stats counters, and configures
// the LEDC PWM peripheral at the default frequency (MPPT_PWM_FREQ_HZ).
// Call once at startup, or after switching MPPT methods. Does NOT itself
// turn the PWM output on - call enablePWM() once the caller is ready to
// start converting (this lets soft-start begin from a clean, deliberate
// point rather than firmware boot).
void initMPPT(float startingDutyCycle);

// --- PWM frequency configuration ---

// Reconfigures the LEDC timer to a new switching frequency at runtime
// (e.g. to trade efficiency vs. inductor/capacitor sizing for a
// different converter topology) without disturbing the currently
// commanded duty cycle.
void setPWMFrequency(uint32_t freqHz);
uint32_t getPWMFrequency();

// --- PWM enable / disable ---

// Enables the PWM output and begins a soft-start ramp: duty climbs from 0%
// up to the last known/target duty cycle over MPPT_SOFT_START_DURATION_MS,
// instead of snapping straight to an arbitrary duty. Safe to call again
// while already enabled (restarts the ramp from the current duty).
void enablePWM();

// Immediately disables the PWM output (duty forced to 0%, LEDC channel
// silenced) and cancels any in-progress soft start. Used both for a
// deliberate shutdown and automatically by updateMPPT() when a shutdown
// fault is active.
void disablePWM();

bool isPWMEnabled();

// --- Sensor filtering ---

// Low-pass (EMA) filters a raw SolarData sample's voltage and current,
// recomputing power from the filtered values. Smooths ADC/sensor noise
// out of the signal the MPPT algorithms make perturbation decisions on.
// Call resetSolarFilter() whenever the filter should snap to a fresh
// reading instead of blending in the old value (e.g. right after
// re-enabling PWM after a fault, so the filter doesn't drag the stale
// pre-fault value into the new ramp).
SolarData filterSolarData(SolarData raw);
void resetSolarFilter();

// --- MPPT algorithms (operate on already-filtered SolarData) ---

// Perturb & Observe: nudges the duty cycle each call and watches whether
// power increased or decreased, continuing in whichever direction helped
// last time (or reversing if it didn't). The perturbation size is
// adaptive (see adaptiveStepSize below), not a fixed constant.
MPPTResult runPerturbAndObserve(SolarData solar);

// Incremental Conductance: compares instantaneous conductance (I/V)
// against incremental conductance (dI/dV). At the true maximum power
// point, dI/dV == -I/V exactly, so this can detect the MPP directly
// rather than only inferring direction from the previous step's result.
// Also uses an adaptive perturbation size.
MPPTResult runIncrementalConductance(SolarData solar);

// Applies a duty cycle (0.0-1.0) to the MPPT PWM output pin. Internal
// bookkeeping only - prefer updateMPPT() from application code, which
// routes every duty change through the slew-rate limiter and the
// enable/soft-start/fault-shutdown state machine below.
void applyDutyCycle(float duty);

// --- Top-level orchestrator ---

// Single entry point main.cpp calls once per loop() iteration. Handles,
// in order:
//   1. Automatic PWM shutdown/recovery based on activeFault.
//   2. Fixed-interval gating - the MPPT algorithm itself only actually
//      runs every MPPT_INTERVAL_MS, regardless of how often loop() spins,
//      using an adaptive step size scaled to how far off the MPP looks.
//   3. Sensor filtering of the raw SolarData before it reaches MPPT.
//   4. Soft-start ramp (if a start/restart is in progress).
//   5. Duty cycle slew-rate limiting before the value is ever written
//      to the PWM peripheral.
//   6. Updates diagnostics/statistics counters and, at most once every
//      MPPT_STATS_LOG_INTERVAL_MS, logs a performance summary.
// Returns the MPPTResult actually applied this call (duty, filtered
// voltage reference, and whether the converter is currently active).
MPPTResult updateMPPT(MPPTMethod method, SolarData rawSolar, FaultCode activeFault, unsigned long nowMs);

// True if this fault code should force an automatic PWM shutdown.
// Covers overcurrent, overvoltage, undervoltage, overtemperature,
// battery-not-detected, and communication timeout.
bool isPWMShutdownFault(FaultCode fault);

// --- Diagnostics & statistics ---

// Point-in-time snapshot of everything about the converter's current
// state - safe to call as often as desired (e.g. on an upstream request
// frame), does not mutate any tracking state itself.
MPPTDiagnostics getMPPTDiagnostics(unsigned long nowMs);

// Logs one line summarizing current diagnostics via DebugLog.
void printMPPTDiagnostics(unsigned long nowMs);

MPPTStats getMPPTStats();
void resetMPPTStats();

// Logs one line summarizing cumulative performance stats via DebugLog.
void logMPPTStats();

#endif

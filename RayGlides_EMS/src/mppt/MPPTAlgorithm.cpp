#include "MPPTAlgorithm.h"
#include "config.h"
#include "../debug/DebugLog.h"
#include "thermal/ThermalManager.h"

// --- Algorithm tracking state (Perturb & Observe / Incremental Conductance) ---
static float prevVoltage = 0;
static float prevCurrent = 0;
static float prevPower = 0;
static float dutyCycle = 0.5;  // Algorithm's current target duty, pre-slew/pre-soft-start
static float lastStepSize = 0.0;  // Adaptive perturbation size used on the most recent decision
static MPPTMethod lastMethod = MPPT_PERTURB_OBSERVE;

// --- PWM output state ---
static float    commandedDuty = 0.0;   // Actual duty last written to hardware (post-slew)
static bool     pwmEnabled = false;
static bool     wasShutdownByFault = false;  // Distinguishes an auto fault-shutdown from a deliberate disablePWM()
static FaultCode lastShutdownFault = FAULT_NONE;  // Which fault (if any) is currently forcing the shutdown
static uint32_t currentPWMFreqHz = MPPT_PWM_FREQ_HZ;

// --- Soft-start ramp state ---
static bool          softStartActive = false;
static unsigned long softStartBeginMs = 0;
static float          softStartFromDuty = 0.0;
static float          softStartTargetDuty = 0.0;

// --- Slew-rate limiter state ---
static unsigned long lastSlewUpdateMs = 0;
static bool           lastWriteSlewLimited = false;

// --- Fixed-interval MPPT decision scheduling ---
static unsigned long lastMPPTRunMs = 0;
static bool           mpptHasRun = false;

// --- Sensor filter (EMA) state ---
static float filtVoltage = 0;
static float filtCurrent = 0;
static bool  filterInitialized = false;

// --- Performance statistics ---
static unsigned long statsEnabledTimeMs = 0;
static unsigned long statsFaultShutdownCount = 0;
static unsigned long statsSoftStartCount = 0;
static unsigned long statsMpptDecisionCount = 0;
static unsigned long statsSlewLimitedCount = 0;
static float          statsMinDuty = 1.0;
static float          statsMaxDuty = 0.0;
static float          statsAvgDuty = 0.0;
static unsigned long statsDutySampleCount = 0;
static unsigned long lastStatsLogMs = 0;
static unsigned long lastStatsUpdateMs = 0;

static float clampDuty(float d) {
  if (d > MPPT_DUTY_MAX) return MPPT_DUTY_MAX;
  if (d < MPPT_DUTY_MIN) return MPPT_DUTY_MIN;
  return d;
}

// Scales the perturbation step with how far off the last reading looks:
// a big swing in power gets a big step (converge fast after a cloud edge
// or a sudden load change), a near-zero swing gets the smallest step
// (settle quietly right at the MPP instead of dithering around it).
static float adaptiveStepSize(float deltaP) {
  float magnitude = fabs(deltaP);
  float ratio = magnitude / MPPT_ADAPTIVE_POWER_SCALE;
  if (ratio > 1.0f) ratio = 1.0f;
  float step = MPPT_DUTY_STEP_MIN + ratio * (MPPT_DUTY_STEP_MAX - MPPT_DUTY_STEP_MIN);
  lastStepSize = step;
  return step;
}

void initMPPT(float startingDutyCycle) {
  dutyCycle = clampDuty(startingDutyCycle);
  prevVoltage = 0;
  prevCurrent = 0;
  prevPower = 0;
  lastStepSize = 0.0;

  commandedDuty = 0.0;
  pwmEnabled = false;
  wasShutdownByFault = false;
  lastShutdownFault = FAULT_NONE;

  softStartActive = false;
  softStartBeginMs = 0;
  softStartFromDuty = 0.0;
  softStartTargetDuty = 0.0;

  lastSlewUpdateMs = 0;
  lastWriteSlewLimited = false;
  lastMPPTRunMs = 0;
  mpptHasRun = false;

  resetSolarFilter();
  resetMPPTStats();
  lastStatsUpdateMs = 0;

  // Bring the LEDC peripheral up at the configured default frequency.
  // setPWMFrequency() can retune this at runtime afterwards.
  currentPWMFreqHz = MPPT_PWM_FREQ_HZ;
  ledcSetup(MPPT_PWM_CHANNEL, currentPWMFreqHz, MPPT_PWM_RESOLUTION_BITS);
  ledcAttachPin(MPPT_PWM_PIN, MPPT_PWM_CHANNEL);
}

// --- PWM frequency configuration ---

void setPWMFrequency(uint32_t freqHz) {
  if (freqHz == 0) return;  // guard against an obviously invalid request
  currentPWMFreqHz = freqHz;
  ledcSetup(MPPT_PWM_CHANNEL, currentPWMFreqHz, MPPT_PWM_RESOLUTION_BITS);
  // Re-applying the current duty immediately reflects it on the
  // reconfigured timer instead of leaving the output at whatever the
  // fresh ledcSetup() call reset it to.
  applyDutyCycle(commandedDuty);
}

uint32_t getPWMFrequency() {
  return currentPWMFreqHz;
}

// --- PWM enable / disable ---

// Shared soft-start kickoff, timestamped explicitly so both the public
// enablePWM() (real-time callers, uses millis()) and updateMPPT()'s
// automatic fault-recovery path (already threading a nowMs) agree on the
// same clock reading - mixing an internal millis() call with a caller-
// supplied nowMs would let the two drift apart and corrupt the ramp timing.
static void beginSoftStart(unsigned long nowMs) {
  softStartFromDuty = commandedDuty;   // Usually 0.0 coming from a disabled state
  softStartTargetDuty = dutyCycle;      // Ramp up to wherever the algorithm currently wants to be
  softStartBeginMs = nowMs;
  softStartActive = true;
  pwmEnabled = true;
  lastSlewUpdateMs = nowMs;
  statsSoftStartCount++;

  // Don't blend a fresh restart into whatever the filter last saw pre-shutdown.
  resetSolarFilter();
}

void enablePWM() {
  beginSoftStart(millis());
}

void disablePWM() {
  pwmEnabled = false;
  softStartActive = false;
  commandedDuty = 0.0;
  ledcWrite(MPPT_PWM_CHANNEL, 0);
}

bool isPWMEnabled() {
  return pwmEnabled;
}

// --- Sensor filtering ---

SolarData filterSolarData(SolarData raw) {
  if (!filterInitialized) {
    filtVoltage = raw.voltage;
    filtCurrent = raw.current;
    filterInitialized = true;
  } else {
    filtVoltage += MPPT_FILTER_ALPHA * (raw.voltage - filtVoltage);
    filtCurrent += MPPT_FILTER_ALPHA * (raw.current - filtCurrent);
  }

  SolarData filtered = raw;   // Keep rawVoltage/rawCurrent for diagnostics
  filtered.voltage = filtVoltage;
  filtered.current = filtCurrent;
  filtered.power = filtVoltage * filtCurrent;
  return filtered;
}

void resetSolarFilter() {
  filterInitialized = false;
}

// --- MPPT algorithms ---

MPPTResult runPerturbAndObserve(SolarData solar) {
  float power = solar.power;
  float deltaP = power - prevPower;
  float deltaV = solar.voltage - prevVoltage;
  float step = adaptiveStepSize(deltaP);

  if (fabs(deltaP) > MPPT_POWER_EPSILON) {
    if (deltaP > 0) {
      // Power went up - keep perturbing in the same direction as last time
      if (deltaV > 0) dutyCycle += step;
      else             dutyCycle -= step;
    } else {
      // Power went down - we perturbed the wrong way, reverse
      if (deltaV > 0) dutyCycle -= step;
      else             dutyCycle += step;
    }
  }
  // else: power essentially unchanged - close enough to the MPP, hold steady

  dutyCycle = clampDuty(dutyCycle);

  prevVoltage = solar.voltage;
  prevCurrent = solar.current;
  prevPower = power;

  MPPTResult result;
  result.dutyCycle = dutyCycle;
  result.voltageRef = solar.voltage;
  result.pwmActive = true;
  return result;
}

MPPTResult runIncrementalConductance(SolarData solar) {
  float deltaV = solar.voltage - prevVoltage;
  float deltaI = solar.current - prevCurrent;
  float deltaP = solar.power - prevPower;
  float step = adaptiveStepSize(deltaP);

  if (fabs(deltaV) < MPPT_VOLTAGE_EPSILON) {
    // No meaningful voltage change since last cycle
    if (fabs(deltaI) < MPPT_CURRENT_EPSILON) {
      // dI ~ 0 too - already sitting at the MPP, hold
    } else if (deltaI > 0) {
      dutyCycle += step;   // Irradiance rising - push Vref up
    } else {
      dutyCycle -= step;   // Irradiance falling - pull Vref down
    }
  } else {
    float instantaneousConductance = solar.current / solar.voltage;   // I / V
    float incrementalConductance   = deltaI / deltaV;                  // dI / dV

    if (fabs(incrementalConductance + instantaneousConductance) < MPPT_COND_EPSILON) {
      // dI/dV == -I/V (within tolerance) - sitting right at the MPP, hold
    } else if (incrementalConductance > -instantaneousConductance) {
      dutyCycle += step;   // Left of the MPP - move right
    } else {
      dutyCycle -= step;   // Right of the MPP - move left
    }
  }

  dutyCycle = clampDuty(dutyCycle);

  prevVoltage = solar.voltage;
  prevCurrent = solar.current;
  prevPower = solar.power;

  MPPTResult result;
  result.dutyCycle = dutyCycle;
  result.voltageRef = solar.voltage;
  result.pwmActive = true;
  return result;
}

void applyDutyCycle(float duty) {
  duty = duty * getMPPTPowerDerateFactor();
  duty = clampDuty(duty);
  commandedDuty = duty;
  uint32_t maxCount = (1 << MPPT_PWM_RESOLUTION_BITS) - 1;
  uint32_t pwmValue = (uint32_t)(duty * maxCount);
  ledcWrite(MPPT_PWM_CHANNEL, pwmValue);
}

// --- Fault -> shutdown mapping ---

bool isPWMShutdownFault(FaultCode fault) {
  switch (fault) {
    case F001_BATTERY_NOT_DETECTED:      // Battery fault - can't safely charge what isn't there
    case F002_BATTERY_OVER_VOLTAGE:
    case F003_BATTERY_UNDER_VOLTAGE:
    case F004_BATTERY_OVER_TEMPERATURE:
    case F008_COMM_TIMEOUT:
    case F010_SENSOR_FAILURE:
    case F011_OVER_CURRENT:
    case F012_THERMAL_SHUTDOWN:
    case F013_WATCHDOG_LOCKOUT:
      return true;
    default:
      return false;
  }
}

// --- Statistics helpers ---

static void recordDutyStat(float duty) {
  if (duty < statsMinDuty) statsMinDuty = duty;
  if (duty > statsMaxDuty) statsMaxDuty = duty;
  statsDutySampleCount++;
  // Running mean, updated incrementally so no history needs to be stored.
  statsAvgDuty += (duty - statsAvgDuty) / (float)statsDutySampleCount;
}

void resetMPPTStats() {
  statsEnabledTimeMs = 0;
  statsFaultShutdownCount = 0;
  statsSoftStartCount = 0;
  statsMpptDecisionCount = 0;
  statsSlewLimitedCount = 0;
  statsMinDuty = 1.0;
  statsMaxDuty = 0.0;
  statsAvgDuty = 0.0;
  statsDutySampleCount = 0;
  lastStatsLogMs = 0;
}

MPPTStats getMPPTStats() {
  MPPTStats s;
  s.enabledTimeMs = statsEnabledTimeMs;
  s.faultShutdownCount = statsFaultShutdownCount;
  s.softStartCount = statsSoftStartCount;
  s.mpptDecisionCount = statsMpptDecisionCount;
  s.slewLimitedCount = statsSlewLimitedCount;
  s.minDutyObserved = (statsDutySampleCount > 0) ? statsMinDuty : 0.0f;
  s.maxDutyObserved = statsMaxDuty;
  s.avgDutyObserved = statsAvgDuty;
  return s;
}

void logMPPTStats() {
  MPPTStats s = getMPPTStats();
  char msg[160];
  snprintf(msg, sizeof(msg),
    "onTime=%lus decisions=%lu softStarts=%lu faultShutdowns=%lu slewLimited=%lu duty[min=%.0f%% avg=%.0f%% max=%.0f%%]",
    s.enabledTimeMs / 1000UL, s.mpptDecisionCount, s.softStartCount, s.faultShutdownCount,
    s.slewLimitedCount, s.minDutyObserved * 100.0f, s.avgDutyObserved * 100.0f, s.maxDutyObserved * 100.0f);
  logInfo("MPPT_STATS", msg);
}

// --- Diagnostics ---

MPPTDiagnostics getMPPTDiagnostics(unsigned long nowMs) {
  MPPTDiagnostics d;
  d.dutyCycle = commandedDuty;
  d.pwmFrequencyHz = currentPWMFreqHz;
  d.pwmEnabled = pwmEnabled;
  d.pwmActive = pwmEnabled && !wasShutdownByFault;
  d.softStartActive = softStartActive;
  if (softStartActive && MPPT_SOFT_START_DURATION_MS > 0) {
    unsigned long elapsed = nowMs - softStartBeginMs;
    float progress = (float)elapsed / (float)MPPT_SOFT_START_DURATION_MS;
    d.softStartProgress = (progress < 0.0f) ? 0.0f : ((progress > 1.0f) ? 1.0f : progress);
  } else {
    d.softStartProgress = softStartActive ? 0.0f : 1.0f;
  }
  d.method = lastMethod;
  d.shutdownFault = lastShutdownFault;
  d.filteredVoltage = filtVoltage;
  d.filteredCurrent = filtCurrent;
  d.filteredPower = filtVoltage * filtCurrent;
  d.algorithmTargetDuty = softStartActive ? softStartTargetDuty : dutyCycle;
  d.lastStepSize = lastStepSize;
  d.slewLimited = lastWriteSlewLimited;
  d.msSinceLastMPPTRun = mpptHasRun ? (nowMs - lastMPPTRunMs) : 0;
  return d;
}

void printMPPTDiagnostics(unsigned long nowMs) {
  MPPTDiagnostics d = getMPPTDiagnostics(nowMs);
  char msg[200];
  snprintf(msg, sizeof(msg),
    "duty=%.1f%% freq=%luHz pwm=%s%s method=%s Vf=%.2f If=%.2f Pf=%.2f step=%.3f slewLimited=%s lastRun=%lums ago",
    d.dutyCycle * 100.0f, (unsigned long)d.pwmFrequencyHz,
    d.pwmActive ? "ACTIVE" : (d.pwmEnabled ? "ENABLED" : "OFF"),
    d.softStartActive ? " (soft-start)" : "",
    (d.method == MPPT_PERTURB_OBSERVE) ? "P&O" : "IncCond",
    d.filteredVoltage, d.filteredCurrent, d.filteredPower,
    d.lastStepSize, d.slewLimited ? "yes" : "no", d.msSinceLastMPPTRun);
  logInfo("MPPT_DIAG", msg);
}

// --- Top-level orchestrator ---

MPPTResult updateMPPT(MPPTMethod method, SolarData rawSolar, FaultCode activeFault, unsigned long nowMs) {
  MPPTResult result;
  lastMethod = method;

  // --- Statistics: accumulate enabled runtime and periodically log a summary ---
  if (pwmEnabled && lastStatsUpdateMs != 0) {
    statsEnabledTimeMs += (nowMs - lastStatsUpdateMs);
  }
  lastStatsUpdateMs = nowMs;
  if (lastStatsLogMs == 0) lastStatsLogMs = nowMs;
  if ((nowMs - lastStatsLogMs) >= (unsigned long)MPPT_STATS_LOG_INTERVAL_MS) {
    logMPPTStats();
    lastStatsLogMs = nowMs;
  }

  // --- 1. Automatic shutdown / recovery ---
  if (isPWMShutdownFault(activeFault)) {
    if (pwmEnabled) {
      disablePWM();
      statsFaultShutdownCount++;
    }
    wasShutdownByFault = true;
    lastShutdownFault = activeFault;

    result.dutyCycle = 0.0;
    result.voltageRef = rawSolar.voltage;
    result.pwmActive = false;
    return result;
  }

  if (wasShutdownByFault && !pwmEnabled) {
    // The fault that shut us down has cleared - resume automatically, softly.
    beginSoftStart(nowMs);
    wasShutdownByFault = false;
    lastShutdownFault = FAULT_NONE;
  }

  if (!pwmEnabled) {
    // Disabled deliberately (not by a fault) - stay off until explicitly re-enabled.
    result.dutyCycle = 0.0;
    result.voltageRef = rawSolar.voltage;
    result.pwmActive = false;
    return result;
  }

  // --- 2. Filter sensor readings before they ever reach the MPPT algorithm ---
  SolarData filtered = filterSolarData(rawSolar);

  // --- 3. Soft-start ramp takes priority over normal tracking while active ---
  float target;
  if (softStartActive) {
    unsigned long elapsed = nowMs - softStartBeginMs;
    if (elapsed >= (unsigned long)MPPT_SOFT_START_DURATION_MS) {
      target = softStartTargetDuty;
      softStartActive = false;

      // Hand off to normal MPPT tracking starting from here, so the first
      // real perturbation compares against the readings at hand-off instead
      // of stale pre-ramp values.
      dutyCycle = target;
      prevVoltage = filtered.voltage;
      prevCurrent = filtered.current;
      prevPower = filtered.power;
      lastMPPTRunMs = nowMs;
      mpptHasRun = true;
    } else {
      float progress = (float)elapsed / (float)MPPT_SOFT_START_DURATION_MS;
      target = softStartFromDuty + progress * (softStartTargetDuty - softStartFromDuty);
    }
  } else {
    // --- 4. MPPT decision runs at a fixed interval, independent of loop() cadence ---
    if (!mpptHasRun || (nowMs - lastMPPTRunMs) >= (unsigned long)MPPT_INTERVAL_MS) {
      if (method == MPPT_PERTURB_OBSERVE) {
        runPerturbAndObserve(filtered);
      } else {
        runIncrementalConductance(filtered);
      }
      lastMPPTRunMs = nowMs;
      mpptHasRun = true;
      statsMpptDecisionCount++;
    }
    target = dutyCycle;  // Updated in place by whichever algorithm last ran
  }

  // --- 5. Slew-rate limit the commanded duty before it ever reaches hardware ---
  unsigned long dtMs = nowMs - lastSlewUpdateMs;
  lastSlewUpdateMs = nowMs;
  float dtSeconds = dtMs / 1000.0f;

  float maxDelta = MPPT_DUTY_SLEW_RATE_PER_S * dtSeconds;
  float delta = target - commandedDuty;
  bool limited = false;
  if (delta > maxDelta)  { delta = maxDelta;  limited = true; }
  if (delta < -maxDelta) { delta = -maxDelta; limited = true; }
  lastWriteSlewLimited = limited;
  if (limited) statsSlewLimitedCount++;

  applyDutyCycle(commandedDuty + delta);
  recordDutyStat(commandedDuty);

  result.dutyCycle = commandedDuty;   // The actual, slew-limited value now on the PWM pin
  result.voltageRef = filtered.voltage;
  result.pwmActive = true;
  return result;
}

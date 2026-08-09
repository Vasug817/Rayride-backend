# Pseudocode — Battery Monitoring

Corresponds to: `BatteryMonitor.h/.cpp` (sensor reading) and
`BatteryStateMachine.h/.cpp` (state logic). Fault checks now live in
`../fault/FaultDetection.h/.cpp`, operating on the `BatteryData` this
module produces.

```
MODULE BatteryMonitoring

CONSTANTS
    DETECT_MIN_RAW = 50
    UNDER_VOLT_MIN = 42.0        // volts
    OVER_VOLT_MAX  = 68.0        // volts
    OVER_CURRENT_A = 25.0        // amps (magnitude)
    OVER_TEMP_C    = 60.0        // celsius
    VOLTAGE_DIVIDER_RATIO = 21.0
    CURRENT_SENSOR_MIDPOINT = 1.65   // volts at 0A
    CURRENT_SENSITIVITY_V_PER_A = 0.066
    TEMP_MV_PER_C = 10.0
    FULL_CHARGE_SOC = 100
    RESUME_CHARGE_SOC = 95
    DEBOUNCE_COUNT = 3

STATE
    notDetectedCounter = 0
    overVoltCounter = 0
    underVoltCounter = 0
    overTempCounter = 0
    overCurrentCounter = 0


// --- Sensor reading: raw ADC -> physically meaningful values ---

FUNCTION ReadBatteryData():
    rawVoltage = ReadAnalogPin(BATTERY_VOLTAGE_PIN)
    dividerVoltage = (rawVoltage / ADC_MAX) * ADC_VREF
    voltage = dividerVoltage * VOLTAGE_DIVIDER_RATIO

    rawCurrent = ReadAnalogPin(BATTERY_CURRENT_PIN)
    currentSensorVoltage = (rawCurrent / ADC_MAX) * ADC_VREF
    current = (currentSensorVoltage - CURRENT_SENSOR_MIDPOINT) / CURRENT_SENSITIVITY_V_PER_A

    rawTemp = ReadAnalogPin(BATTERY_TEMP_PIN)
    tempSensorVoltage = (rawTemp / ADC_MAX) * ADC_VREF
    temperature = (tempSensorVoltage * 1000) / TEMP_MV_PER_C

    soc = Clamp((rawVoltage / ADC_MAX) * 100, 0, 100)

    RETURN BatteryData(rawVoltage, voltage, current, temperature, soc)


// --- Fault monitoring: BatteryData -> debounced fault code ---

FUNCTION DetectBatteryFault(data):
    IF data.rawVoltage < DETECT_MIN_RAW:
        notDetectedCounter = notDetectedCounter + 1
    ELSE:
        notDetectedCounter = 0

    IF data.voltage >= OVER_VOLT_MAX:
        overVoltCounter = overVoltCounter + 1
    ELSE:
        overVoltCounter = 0

    IF data.rawVoltage >= DETECT_MIN_RAW AND data.voltage < UNDER_VOLT_MIN:
        underVoltCounter = underVoltCounter + 1
    ELSE:
        underVoltCounter = 0

    IF data.temperature >= OVER_TEMP_C:
        overTempCounter = overTempCounter + 1
    ELSE:
        overTempCounter = 0

    IF ABS(data.current) >= OVER_CURRENT_A:
        overCurrentCounter = overCurrentCounter + 1
    ELSE:
        overCurrentCounter = 0

    IF notDetectedCounter >= DEBOUNCE_COUNT:  RETURN (F001_NOT_DETECTED, CRITICAL)
    IF overVoltCounter >= DEBOUNCE_COUNT:     RETURN (F002_OVER_VOLTAGE, CRITICAL)
    IF overTempCounter >= DEBOUNCE_COUNT:     RETURN (F004_OVER_TEMPERATURE, CRITICAL)
    IF overCurrentCounter >= DEBOUNCE_COUNT:  RETURN (F011_OVER_CURRENT, CRITICAL)
    IF underVoltCounter >= DEBOUNCE_COUNT:    RETURN (F003_UNDER_VOLTAGE, WARNING)

    RETURN (NO_FAULT, NONE)


// --- State machine: SOC + fault criticality -> charge state ---

FUNCTION UpdateChargeState(currentState, soc, hasCriticalFault):
    IF hasCriticalFault:
        RETURN STATE_FAULT

    IF currentState == STATE_FAULT:
        RETURN STATE_IDLE   // re-evaluate fresh after recovery

    IF currentState == STATE_IDLE:
        RETURN (soc >= FULL_CHARGE_SOC) ? STATE_FULLY_CHARGED : STATE_CHARGING

    IF currentState == STATE_CHARGING:
        IF soc >= FULL_CHARGE_SOC:
            RETURN STATE_FULLY_CHARGED
        RETURN STATE_CHARGING

    IF currentState == STATE_FULLY_CHARGED:
        IF soc < RESUME_CHARGE_SOC:
            RETURN STATE_CHARGING
        RETURN STATE_FULLY_CHARGED

    RETURN currentState


// --- EMS integration: how this module's output reaches the rest of the system ---
// main.cpp calls ReadBatteryData() once per cycle, passes the result into
// DetectBatteryFault() to get the active fault, passes soc + fault
// criticality into UpdateChargeState() to drive the state machine, and
// passes the full BatteryData into RelayControl (via the fault + state
// it produced) and into the RayGlidesProtocol status frame - so voltage,
// current, and temperature are visible system-wide, not just inside this
// module.
```

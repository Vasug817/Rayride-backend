# Pseudocode — Solar Monitoring

Corresponds to: `SolarMonitor.h/.cpp` (sensor reading), fault checks in
`../fault/FaultDetection.h/.cpp`, and consumption in
`../charging/ChargingDecision.h/.cpp`.

```
MODULE SolarMonitoring

CONSTANTS
    SOLAR_VOLTAGE_DIVIDER_RATIO = 8.0
    SOLAR_CURRENT_SENSITIVITY = 0.185      // volts per amp (ACS712 5A-style)
    CURRENT_SENSOR_MIDPOINT = 1.65          // volts at 0A
    SOLAR_FAULT_MIN_CURRENT = 0.3           // amps
    SOLAR_MAX_PLAUSIBLE_RAW = 4090
    SOLAR_SUFFICIENT_W = 150.0
    SOLAR_USABLE_W = 20.0
    DEBOUNCE_COUNT = 3

STATE
    solarFaultCounter = 0


// --- Sensor reading: raw ADC -> physically meaningful values ---

FUNCTION ReadSolarData():
    rawVoltage = ReadAnalogPin(SOLAR_VOLTAGE_PIN)
    dividerVoltage = (rawVoltage / ADC_MAX) * ADC_VREF
    voltage = dividerVoltage * SOLAR_VOLTAGE_DIVIDER_RATIO

    rawCurrent = ReadAnalogPin(SOLAR_CURRENT_PIN)
    currentSensorVoltage = (rawCurrent / ADC_MAX) * ADC_VREF
    current = (currentSensorVoltage - CURRENT_SENSOR_MIDPOINT) / SOLAR_CURRENT_SENSITIVITY
    IF current < 0:
        current = 0            // panels don't sink current in this model

    power = voltage * current

    RETURN SolarData(rawVoltage, rawCurrent, voltage, current, power)


// --- Fault monitoring: implausible readings, NOT simply "no sun" ---

FUNCTION DetectSolarFault(data):
    // Current flowing with ~0V present is electrically implausible
    implausible = (data.voltage < 1.0 AND data.current >= SOLAR_FAULT_MIN_CURRENT)

    // Raw reading pinned at the top of the ADC range suggests a wiring
    // fault rather than genuinely strong sun (real panels taper, they
    // don't sit flat at the rail)
    pinnedMax = (data.rawVoltage >= SOLAR_MAX_PLAUSIBLE_RAW)

    IF implausible OR pinnedMax:
        solarFaultCounter = solarFaultCounter + 1
    ELSE:
        solarFaultCounter = 0

    IF solarFaultCounter >= DEBOUNCE_COUNT:
        RETURN (F005_SOLAR_FAULT, WARNING)

    RETURN (NO_FAULT, NONE)

    // Note: F005 is a Warning, not Critical - a solar fault should not
    // halt the battery state machine or open the relay, since the system
    // can fall back to Grid Only. Only battery-side faults are Critical.


// --- Consumption: SolarData -> charging mode decision ---

FUNCTION DecideChargingMode(solarData, gridAvailable):
    IF solarData.power >= SOLAR_SUFFICIENT_W:
        RETURN MODE_SOLAR_ONLY
    ELSE IF solarData.power >= SOLAR_USABLE_W AND gridAvailable:
        RETURN MODE_HYBRID
    ELSE IF gridAvailable:
        RETURN MODE_GRID_ONLY
    ELSE IF solarData.power >= SOLAR_USABLE_W:
        RETURN MODE_SOLAR_ONLY
    RETURN MODE_NO_CHARGE


// --- EMS integration ---
// main.cpp calls ReadSolarData() once per cycle, passes the result into
// DetectSolarFault() (combined with battery/comm faults using priority:
// battery > solar > communication), and passes the same SolarData into
// DecideChargingMode() alongside grid availability. Solar voltage and
// power are also included in the RayGlidesProtocol status frame, so
// solar health is visible system-wide alongside battery telemetry.
```

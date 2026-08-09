# Pseudocode — Relay Control

Corresponds to: `RelayControl.h/.cpp`

```
MODULE RelayControl

// The relay is the physical switch that enables/disables battery charging.
// It must NEVER be energized while a fault is active, regardless of what
// the charging state machine says - this is a safety interlock layered
// on top of the state machine's own logic.

FUNCTION UpdateRelay(chargeState, activeFault):
    IF activeFault != NO_FAULT AND Severity(activeFault) == CRITICAL:
        SetRelay(OPEN)              // Force relay open - safety first
        SetIndicatorLED(FAULT_LED, ON)
        SetIndicatorLED(CHARGE_LED, OFF)
        RETURN

    IF chargeState == STATE_CHARGING:
        SetRelay(CLOSED)             // Energize relay - allow current to flow
        SetIndicatorLED(CHARGE_LED, ON)
        SetIndicatorLED(FAULT_LED, OFF)

    ELSE IF chargeState == STATE_FULLY_CHARGED:
        SetRelay(OPEN)               // Stop charging, battery is full
        SetIndicatorLED(CHARGE_LED, OFF)
        SetIndicatorLED(FAULT_LED, OFF)

    ELSE IF chargeState == STATE_IDLE:
        SetRelay(OPEN)               // Not yet charging
        SetIndicatorLED(CHARGE_LED, OFF)
        SetIndicatorLED(FAULT_LED, OFF)

    ELSE IF chargeState == STATE_FAULT:
        SetRelay(OPEN)               // Redundant safety - covered above too
        SetIndicatorLED(FAULT_LED, ON)
        SetIndicatorLED(CHARGE_LED, OFF)


FUNCTION SetRelay(position):
    IF position == CLOSED:
        WriteDigitalPin(RELAY_PIN, HIGH)
    ELSE:
        WriteDigitalPin(RELAY_PIN, LOW)
```

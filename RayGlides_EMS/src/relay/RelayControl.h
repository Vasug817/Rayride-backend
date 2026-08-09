#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <Arduino.h>
#include "../battery/BatteryStateMachine.h"
#include "../fault/FaultDetection.h"

enum RelayPosition { RELAY_OPEN, RELAY_CLOSED };

void initRelay();

// Translates charging state + active fault into a physical relay position
// and updates the charge/fault indicator LEDs. Enforces three hard safety
// interlocks, checked in this order: the relay is always forced open
// during an active OTA transfer, always forced open during a watchdog
// crash-loop lockout, and always forced open when a critical fault is
// active - all three regardless of what the charging state machine reports.
void updateRelay(ChargeState chargeState, FaultCode activeFault);

#endif

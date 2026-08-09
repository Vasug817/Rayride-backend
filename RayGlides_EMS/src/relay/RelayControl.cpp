#include "RelayControl.h"
#include "config.h"
#include "../watchdog/WatchdogRecovery.h"
#include "../ota/OTAUpdate.h"

static void setRelay(RelayPosition position) {
  digitalWrite(RELAY_PIN, position == RELAY_CLOSED ? HIGH : LOW);
}

void initRelay() {
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(RELAY_OPEN);  // Always boot with the relay safely open
}

void updateRelay(ChargeState chargeState, FaultCode activeFault) {
  // --- Safety interlock: an active OTA transfer always wins, over everything ---
  if (isOTAInProgress()) {
    setRelay(RELAY_OPEN);
    digitalWrite(FAULT_LED_PIN, HIGH);
    digitalWrite(CHARGE_LED_PIN, LOW);
    return;
  }

  // --- Safety interlock: crash-loop lockout always wins ---
  if (isWatchdogLockedOut()) {
    setRelay(RELAY_OPEN);
    digitalWrite(FAULT_LED_PIN, HIGH);
    digitalWrite(CHARGE_LED_PIN, LOW);
    return;
  }

  // --- Safety interlock: critical fault always wins ---
  if (activeFault != FAULT_NONE && isCriticalFault(activeFault)) {
    setRelay(RELAY_OPEN);
    digitalWrite(FAULT_LED_PIN, HIGH);
    digitalWrite(CHARGE_LED_PIN, LOW);
    return;
  }

  switch (chargeState) {
    case STATE_CHARGING:
      setRelay(RELAY_CLOSED);
      digitalWrite(CHARGE_LED_PIN, HIGH);
      digitalWrite(FAULT_LED_PIN, LOW);
      break;

    case STATE_FULLY_CHARGED:
      setRelay(RELAY_OPEN);
      digitalWrite(CHARGE_LED_PIN, LOW);
      digitalWrite(FAULT_LED_PIN, LOW);
      break;

    case STATE_IDLE:
      setRelay(RELAY_OPEN);
      digitalWrite(CHARGE_LED_PIN, LOW);
      digitalWrite(FAULT_LED_PIN, LOW);
      break;

    case STATE_FAULT:
      setRelay(RELAY_OPEN);
      digitalWrite(FAULT_LED_PIN, HIGH);
      digitalWrite(CHARGE_LED_PIN, LOW);
      break;
  }
}

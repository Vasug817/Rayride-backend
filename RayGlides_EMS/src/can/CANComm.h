#ifndef CAN_COMM_H
#define CAN_COMM_H

#include <Arduino.h>
#include "../battery/BatteryMonitor.h"
#include "../battery/BatteryStateMachine.h"
#include "../solar/SolarMonitor.h"
#include "../fault/FaultDetection.h"

struct CANReceivedMessage {
  bool valid;
  uint32_t id;
  uint8_t data[8];
  uint8_t length;
};

// Installs and starts the ESP32's TWAI (CAN) driver at 500 kbit/s on
// CAN_TX_PIN/CAN_RX_PIN. Call once from setup().
bool initCAN();

// Sends full battery + solar status as a single CAN frame (ID CAN_ID_STATUS).
// Payload layout matches RayGlidesProtocol's STATUS_UPDATE for consistency:
// [SOC, ChargeState, BattV, BattI(signed), BattT, SolarV, SolarPower] - 7
// bytes, fits within CAN's 8-byte data limit.
void sendCANStatus(BatteryData battery, SolarData solar, ChargeState state);

// Sends a fault report as a CAN frame (ID CAN_ID_FAULT): [FaultCode, Severity]
void sendCANFault(FaultCode code, Severity sev);

// Non-blocking receive: call every loop cycle. Returns valid=false if
// nothing is waiting.
CANReceivedMessage receiveCAN();

#endif

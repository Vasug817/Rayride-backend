#ifndef RAYGLIDES_PROTOCOL_H
#define RAYGLIDES_PROTOCOL_H

#include <Arduino.h>
#include "../battery/BatteryStateMachine.h"
#include "../battery/BatteryMonitor.h"
#include "../solar/SolarMonitor.h"
#include "../fault/FaultDetection.h"

#define MSG_STATUS_UPDATE 0x01
#define MSG_FAULT_REPORT  0x02
#define MSG_ACK           0x03  // Generic transport-level ACK - see NackReason / sendAck()
#define MSG_NACK          0x04  // Generic transport-level NACK - see NackReason / sendNack()
#define MSG_SET_SCENARIO  0x05
#define MSG_SET_CONFIG    0x06
#define MSG_GET_STATUS    0x07
#define MSG_SET_MODE      0x08
#define MSG_SET_FAN       0x09
#define MSG_RESTART       0x0A
#define MSG_HIST_TELEMETRY 0x0B
#define MSG_SET_BROKER    0x0C
#define MSG_SYNC_STATUS   0x0F
#define MSG_OTA_BEGIN     0x10
#define MSG_OTA_DATA      0x11
#define MSG_OTA_END       0x12
#define MSG_OTA_ABORT     0x13
#define MSG_OTA_ACK       0x14
#define FRAME_START 0xAA
#define FRAME_END   0x55
#define MAX_PAYLOAD 32

struct ReceivedFrame {
  bool valid;
  uint8_t msgId;
  uint8_t payload[MAX_PAYLOAD];
  uint8_t length;
};

// Shared signature of sendFrame() (USB Serial) and rs485SendFrame()
// (RS485) - lets transport-agnostic code (like OTA) send a reply on
// whichever bus a request arrived on, without knowing which one it is.
typedef void (*FrameSender)(uint8_t msgId, uint8_t* payload, uint8_t len);

// Reasons a generic MSG_NACK was sent, carried in payload[0]. Distinct
// from OTAAckStatus (OTAUpdate.h) - OTA already has its own richer
// ACK/error vocabulary for the OTA_BEGIN/DATA/END handshake; these
// reasons only cover the generic USB-Serial receive path (non-OTA
// frames arriving from the host, e.g. future host->device commands).
enum NackReason {
  NACK_CHECKSUM_ERROR   = 1,  // Frame's checksum didn't match its payload
  NACK_MALFORMED_FRAME  = 2,  // Bad length or missing FRAME_END
  NACK_UNKNOWN_MSG_ID   = 3,  // Structurally valid frame, but msgId isn't handled
  NACK_INVALID_VALUE    = 4   // Parameter value out of bounds
};

// --- Sending ---
uint8_t computeChecksum(uint8_t msgId, uint8_t len, uint8_t* payload);
void sendFrame(uint8_t msgId, uint8_t* payload, uint8_t len);

// Reports SOC, charge state, battery V/I/T, and solar V/power - full
// system telemetry, not just a single percentage.
void sendStatusUpdate(BatteryData battery, SolarData solar, ChargeState state);
void sendFaultReport(FaultCode code, Severity sev);

// Generic transport ACK/NACK for the USB-Serial receive path (§ below).
// payload: sendAck -> [ackedMsgId]; sendNack -> [reason, ackedMsgId].
// Both go out through sendFrame(), so they share its binary framing and
// its debug trace line - they never touch the text debug log directly.
void sendAck(uint8_t ackedMsgId);
void sendNack(NackReason reason, uint8_t ackedMsgId);

// --- Receiving ---
// Non-blocking: call every loop cycle. Returns a frame with valid=false
// if nothing complete has arrived yet, or if the frame failed validation.
// On a checksum failure or a malformed frame (bad length / missing
// FRAME_END), this also raises F009 internally (existing behavior,
// unchanged) AND sends a MSG_NACK back to the host with the matching
// NackReason - unless an OTA transfer is currently in progress, in
// which case the NACK is suppressed so it can't interleave with an
// in-flight OTA_DATA stream; OTA failures are still fully covered by
// OTAUpdate's own MSG_OTA_ACK error statuses.
ReceivedFrame receiveFrame();

// Dispatches one already-validated, non-OTA frame (main.cpp routes OTA
// message IDs to handleOTAFrame() instead - see isOTAMessage()). Sends
// MSG_ACK for a recognized, successfully-handled frame, or MSG_NACK
// with NACK_UNKNOWN_MSG_ID for anything else. MSG_ACK/MSG_NACK
// themselves are never re-acknowledged, so a stray echo can't loop.
void handleIncomingFrame(ReceivedFrame frame);

// --- Timeout watchdog ---
void noteMessageReceived();
bool checkCommunicationTimeout();

#endif

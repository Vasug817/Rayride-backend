#ifndef RS485_COMM_H
#define RS485_COMM_H

#include <Arduino.h>
#include "../protocol/RayGlidesProtocol.h"  // Reuses FRAME_START/END, checksum, MSG_ids, ReceivedFrame

// Initializes UART2 for RS485 (via a MAX485-style transceiver) and sets
// the DE/RE direction pin to receive mode. Call once from setup().
void initRS485();

// Sends a framed message over RS485: switches the transceiver to
// transmit (DE HIGH), writes the frame, waits for it to fully clock out,
// then switches back to receive (DE LOW) so the bus is free to listen.
void rs485SendFrame(uint8_t msgId, uint8_t* payload, uint8_t len);

// Non-blocking receive: call every loop cycle. Returns a frame with
// valid=false if nothing complete has arrived yet, or if it failed
// checksum validation.
ReceivedFrame rs485ReceiveFrame();

#endif

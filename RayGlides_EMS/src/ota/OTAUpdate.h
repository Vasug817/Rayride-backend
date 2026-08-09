#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <Arduino.h>
#include "../protocol/RayGlidesProtocol.h"

// ACK status codes sent back to the host in MSG_OTA_ACK payload[0].
enum OTAAckStatus {
  OTA_ACK_OK                = 0,
  OTA_ACK_BUSY               = 1,  // an update is already in progress
  OTA_ACK_TOO_LARGE          = 2,  // image bigger than the free OTA partition
  OTA_ACK_BEGIN_FAILED       = 3,  // Update.begin() itself failed
  OTA_ACK_BAD_SEQUENCE       = 4,  // chunk out of order - transfer aborted, host must restart it
  OTA_ACK_WRITE_FAILED       = 5,  // flash write failed
  OTA_ACK_CRC_MISMATCH       = 6,  // finished image doesn't match the CRC32 given at MSG_OTA_BEGIN
  OTA_ACK_NOT_IN_PROGRESS    = 7,  // DATA/END/ABORT arrived with no BEGIN active
  OTA_ACK_SUCCESS_REBOOTING  = 8   // image verified and written - rebooting into it now
};

// True while a firmware transfer is being received/written. RelayControl
// checks this as a hard interlock: the relay is always forced open during
// an OTA update, regardless of charge state or fault status - the last
// thing this system should do while rewriting its own flash is also be
// driving power electronics.
bool isOTAInProgress();

// Checks whether this boot is running an as-yet-unconfirmed OTA image
// (the previous boot's MSG_OTA_END succeeded but never reached
// WATCHDOG_HEALTHY_UPTIME_MS of stable runtime to confirm it). After
// OTA_MAX_BOOT_ATTEMPTS unconfirmed boots, rolls back to the previous
// partition and reboots into it - a firmware image that can't stay up
// long enough to be confirmed gets treated as bad, automatically.
// Call once, early in setup(), right after initWatchdogRecovery().
void initOTARecovery();

// Call once, after WATCHDOG_HEALTHY_UPTIME_MS of stable uptime (the same
// checkpoint that calls confirmWatchdogHealthy()). Marks the current
// image confirmed-good, so a future boot won't roll it back.
void confirmOTAHealthy();

// True if msgId is one of the MSG_OTA_* request types (BEGIN/DATA/END/
// ABORT). MSG_OTA_ACK is device->host only and is never dispatched here.
// main.cpp checks this to route a frame to handleOTAFrame() instead of
// the normal status/fault handler.
bool isOTAMessage(uint8_t msgId);

// Dispatches one already-validated OTA frame. `sendAck` is the
// transport's own sender (sendFrame for USB Serial, rs485SendFrame for
// RS485) so the ACK goes back out on whichever bus the request arrived
// on. Only one transfer can be in progress at a time, across both buses
// (enforced via OTA_ACK_BUSY) - the flash and the relay are shared, so
// two simultaneous transfers make no sense regardless of transport.
void handleOTAFrame(ReceivedFrame frame, FrameSender sendAck);

#endif

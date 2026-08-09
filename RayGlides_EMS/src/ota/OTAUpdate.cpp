#include "OTAUpdate.h"
#include "config.h"
#include "../debug/DebugLog.h"
#include <EEPROM.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// --- Persisted OTA state (separate EEPROM region from WatchdogState) ---

struct OTAState {
  uint32_t magic;
  uint8_t  pendingValidation;        // 1 = current image hasn't confirmed healthy yet
  uint8_t  previousPartitionSubtype; // esp_partition_subtype_t of the partition to roll back to
  uint8_t  bootAttempts;             // unconfirmed boots since pendingValidation was set
  uint8_t  reserved;
  uint32_t checksum;
};

static uint32_t otaStateChecksum(const OTAState &s) {
  return s.magic + s.pendingValidation + s.previousPartitionSubtype + s.bootAttempts;
}

static void loadOTAState(OTAState &out) {
  EEPROM.get(OTA_STATE_EEPROM_OFFSET, out);
  if (out.magic != OTA_STATE_MAGIC || out.checksum != otaStateChecksum(out)) {
    out.magic = OTA_STATE_MAGIC;
    out.pendingValidation = 0;
    out.previousPartitionSubtype = 0;
    out.bootAttempts = 0;
    out.reserved = 0;
    out.checksum = otaStateChecksum(out);
  }
}

static void saveOTAState(OTAState &s) {
  s.magic = OTA_STATE_MAGIC;
  s.checksum = otaStateChecksum(s);
  EEPROM.put(OTA_STATE_EEPROM_OFFSET, s);
  EEPROM.commit();
}

// --- In-progress transfer state ---

static bool inProgress = false;
static uint32_t expectedSize = 0;
static uint32_t expectedCRC = 0;
static uint32_t bytesWritten = 0;
static uint16_t expectedSeq = 0;
static uint32_t crc32Accum = 0xFFFFFFFF;
static bool debugWasEnabledBeforeOTA = true;

static void crc32AddChunk(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc32Accum ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc32Accum = (crc32Accum >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc32Accum & 1)));
    }
  }
}

static void sendOTAAck(FrameSender sendAck, OTAAckStatus status, uint16_t seq) {
  uint8_t payload[3] = { (uint8_t)status, (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8) };
  sendAck(MSG_OTA_ACK, payload, 3);
}

// Debug logging shares the same UART as OTA framing on the Serial bus -
// plain text mid-transfer would corrupt the binary stream the host is
// parsing. Silence it for the duration, restore it on any exit that
// doesn't end in a reboot.
static void silenceDebugForTransfer() {
  debugWasEnabledBeforeOTA = isDebugEnabled();
  setDebugEnabled(false);
}
static void restoreDebugAfterTransfer() {
  setDebugEnabled(debugWasEnabledBeforeOTA);
}

static void abortTransfer(FrameSender sendAck, OTAAckStatus status, uint16_t seq, const char* reason) {
  Update.abort();
  inProgress = false;
  restoreDebugAfterTransfer();
  logError("OTA", reason);
  sendOTAAck(sendAck, status, seq);
}

// --- Public API ---

bool isOTAInProgress() { return inProgress; }

bool isOTAMessage(uint8_t msgId) {
  return msgId == MSG_OTA_BEGIN || msgId == MSG_OTA_DATA ||
         msgId == MSG_OTA_END   || msgId == MSG_OTA_ABORT;
}

void initOTARecovery() {
  OTAState state;
  loadOTAState(state);
  if (!state.pendingValidation) return;  // Normal boot, nothing to check

  state.bootAttempts++;
  if (state.bootAttempts > OTA_MAX_BOOT_ATTEMPTS) {
    logError("OTA", "New image never confirmed healthy - rolling back");

    const esp_partition_t* prev = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)state.previousPartitionSubtype, NULL);

    state.pendingValidation = 0;
    state.bootAttempts = 0;
    saveOTAState(state);

    if (prev != NULL) {
      esp_ota_set_boot_partition(prev);
    }
    delay(200);
    ESP.restart();
    return;  // unreachable
  }

  saveOTAState(state);  // Record this boot attempt and continue normally -
                         // confirmOTAHealthy() clears pendingValidation if
                         // this boot proves stable.
}

void confirmOTAHealthy() {
  OTAState state;
  loadOTAState(state);
  if (state.pendingValidation) {
    state.pendingValidation = 0;
    state.bootAttempts = 0;
    saveOTAState(state);
    logInfo("OTA", "New image confirmed healthy");
  }
}

void handleOTAFrame(ReceivedFrame frame, FrameSender sendAck) {
  switch (frame.msgId) {

    case MSG_OTA_BEGIN: {
      if (inProgress) { sendOTAAck(sendAck, OTA_ACK_BUSY, 0); return; }
      if (frame.length < 8) { sendOTAAck(sendAck, OTA_ACK_BEGIN_FAILED, 0); return; }

      uint32_t size = (uint32_t)frame.payload[0] | ((uint32_t)frame.payload[1] << 8) |
                      ((uint32_t)frame.payload[2] << 16) | ((uint32_t)frame.payload[3] << 24);
      uint32_t crc  = (uint32_t)frame.payload[4] | ((uint32_t)frame.payload[5] << 8) |
                      ((uint32_t)frame.payload[6] << 16) | ((uint32_t)frame.payload[7] << 24);

      silenceDebugForTransfer();  // Keep the wire clean before Update.begin() even runs

      if (!Update.begin(size, U_FLASH)) {
        bool tooLarge = (Update.getError() == UPDATE_ERROR_SIZE);
        restoreDebugAfterTransfer();
        sendOTAAck(sendAck, tooLarge ? OTA_ACK_TOO_LARGE : OTA_ACK_BEGIN_FAILED, 0);
        return;
      }

      inProgress = true;
      expectedSize = size;
      expectedCRC = crc;
      bytesWritten = 0;
      expectedSeq = 0;
      crc32Accum = 0xFFFFFFFF;

      sendOTAAck(sendAck, OTA_ACK_OK, 0);
      break;
    }

    case MSG_OTA_DATA: {
      if (!inProgress) { sendOTAAck(sendAck, OTA_ACK_NOT_IN_PROGRESS, 0); return; }
      if (frame.length < 2) return;  // Malformed - silently ignore, host will time out and retry

      uint16_t seq = (uint16_t)frame.payload[0] | ((uint16_t)frame.payload[1] << 8);
      if (seq != expectedSeq) {
        abortTransfer(sendAck, OTA_ACK_BAD_SEQUENCE, seq, "Chunk out of order - transfer aborted");
        return;
      }

      uint8_t dataLen = frame.length - 2;
      size_t written = Update.write(&frame.payload[2], dataLen);
      if (written != dataLen) {
        abortTransfer(sendAck, OTA_ACK_WRITE_FAILED, seq, "Flash write failed - transfer aborted");
        return;
      }

      crc32AddChunk(&frame.payload[2], dataLen);
      bytesWritten += dataLen;
      expectedSeq++;
      sendOTAAck(sendAck, OTA_ACK_OK, seq);
      break;
    }

    case MSG_OTA_END: {
      if (!inProgress) { sendOTAAck(sendAck, OTA_ACK_NOT_IN_PROGRESS, 0); return; }

      uint32_t finalCRC = ~crc32Accum;
      if (bytesWritten != expectedSize || finalCRC != expectedCRC) {
        abortTransfer(sendAck, OTA_ACK_CRC_MISMATCH, 0, "CRC/size mismatch - transfer aborted");
        return;
      }

      if (!Update.end(true)) {
        abortTransfer(sendAck, OTA_ACK_WRITE_FAILED, 0, "Update.end() failed - transfer aborted");
        return;
      }

      // Record which partition to roll back to BEFORE rebooting - at
      // this point we are still executing the OLD (currently-running)
      // image, so this correctly captures "the partition to return to
      // if the new one never confirms healthy".
      OTAState state;
      loadOTAState(state);
      state.pendingValidation = 1;
      state.previousPartitionSubtype = esp_ota_get_running_partition()->subtype;
      state.bootAttempts = 0;
      saveOTAState(state);

      inProgress = false;
      sendOTAAck(sendAck, OTA_ACK_SUCCESS_REBOOTING, 0);
      delay(200);  // Let the ACK actually clock out over UART before reset
      ESP.restart();
      break;
    }

    case MSG_OTA_ABORT: {
      if (inProgress) {
        Update.abort();
        inProgress = false;
        restoreDebugAfterTransfer();
        logWarn("OTA", "Update aborted by host");
      }
      sendOTAAck(sendAck, OTA_ACK_OK, 0);
      break;
    }
  }
}

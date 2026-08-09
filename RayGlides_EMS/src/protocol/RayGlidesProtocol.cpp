#include "RayGlidesProtocol.h"
#include "config.h"
#include "../debug/DebugLog.h"
#include "../ota/OTAUpdate.h"  // isOTAInProgress() - suppresses generic NACKs mid-transfer
#include "sensors/SensorSimulator.h"
#include "configuration/ConfigManager.h"
#include "charging/ChargingDecision.h"
#include <Preferences.h>

static unsigned long lastMessageTimestamp = 0;

// --- Sending ---

uint8_t computeChecksum(uint8_t msgId, uint8_t len, uint8_t* payload) {
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  uint8_t cs = msgId ^ len;
  for (int i = 0; i < len; i++) cs ^= payload[i];
  return cs;
}

void sendFrame(uint8_t msgId, uint8_t* payload, uint8_t len) {
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  uint8_t checksum = computeChecksum(msgId, len, payload);


  // Real binary wire format - START, msgId, len, payload..., checksum, END -
  // written as raw bytes via Serial.write(), NOT printed as ASCII/HEX text.
  // This must exactly match what receiveFrame() below (and the Raspberry
  // Pi FrameParser) parse byte-for-byte.
  uint8_t frame[3 + MAX_PAYLOAD + 2];
  uint8_t idx = 0;
  frame[idx++] = FRAME_START;
  frame[idx++] = msgId;
  frame[idx++] = len;
  for (int i = 0; i < len; i++) frame[idx++] = payload[i];
  frame[idx++] = checksum;
  frame[idx++] = FRAME_END;
  Serial.write(frame, idx);

  // Human-readable trace goes through the debug logger, not the wire -
  // this keeps binary framing and text diagnostics on genuinely separate
  // channels (logInfo() also auto-silences during an active OTA transfer,
  // which matters since OTA reuses this same sendFrame() for its ACKs).
  char msg[32];
  snprintf(msg, sizeof(msg), "TX msgId=0x%02X len=%d", msgId, len);
  logInfo("PROTO", msg);
}

void sendStatusUpdate(BatteryData battery, SolarData solar, ChargeState state) {
  // Payload: [SOC, ChargeState, BattV, BattI(signed), BattT, SolarV, SolarPower, BattSOH, PrimaryFaultCode]
  uint8_t payload[9] = {
    (uint8_t)battery.soc,
    (uint8_t)state,
    (uint8_t)constrain((int)round(battery.voltage), 0, 255),
    (uint8_t)(int8_t)constrain((int)round(battery.current), -128, 127),
    (uint8_t)constrain((int)round(battery.temperature), 0, 255),
    (uint8_t)constrain((int)round(solar.voltage), 0, 255),
    (uint8_t)constrain((int)round(solar.power), 0, 255),
    (uint8_t)constrain(battery.soh, 0, 255),
    (uint8_t)currentPrimaryFault
  };
  sendFrame(MSG_STATUS_UPDATE, payload, 9);
}

void sendFaultReport(FaultCode code, Severity sev) {
  uint8_t payload[2] = { (uint8_t)code, (uint8_t)sev };
  sendFrame(MSG_FAULT_REPORT, payload, 2);
}

void sendAck(uint8_t ackedMsgId) {
  uint8_t payload[1] = { ackedMsgId };
  sendFrame(MSG_ACK, payload, 1);
}

void sendNack(NackReason reason, uint8_t ackedMsgId) {
  uint8_t payload[2] = { (uint8_t)reason, ackedMsgId };
  sendFrame(MSG_NACK, payload, 2);
}

// Shared by every rejection path in receiveFrame(): suppresses the NACK
// during an active OTA transfer (see header doc) and keeps the log
// message + wire NACK consistent in one place instead of duplicated at
// each call site.
static void rejectFrame(const char* logMsg, NackReason reason, uint8_t msgId) {
  logWarn("PROTO", logMsg);
  if (!isOTAInProgress()) {
    sendNack(reason, msgId);
  }
}

// --- Receiving ---
// Non-blocking: only attempts a read if a start byte is already waiting.
// Intended for a real second device (dashboard/BMS) transmitting frames
// back to the EMS. In a single-device Wokwi simulation this will simply
// find nothing to read, which is expected.

ReceivedFrame receiveFrame() {
  ReceivedFrame result;
  result.valid = false;
  result.length = 0;

  if (Serial.available() < 3) return result;
  if (Serial.peek() != FRAME_START) {
    Serial.read();  // Discard stray byte, resync on next call
    return result;
  }

  uint8_t start = Serial.read();
  uint8_t msgId = Serial.read();
  uint8_t length = Serial.read();

  if (length > MAX_PAYLOAD) {  // Malformed - reject
    rejectFrame("Malformed frame - length exceeds MAX_PAYLOAD", NACK_MALFORMED_FRAME, msgId);
    return result;
  }

  unsigned long startWait = millis();
  while ((uint8_t)Serial.available() < (uint8_t)(length + 2)) {
    if (millis() - startWait > 10) {  // 10ms timeout
      rejectFrame("Malformed frame - receive timeout", NACK_MALFORMED_FRAME, msgId);
      return result;
    }
    delay(1);
  }

  uint8_t payload[MAX_PAYLOAD];
  for (int i = 0; i < length; i++) payload[i] = Serial.read();
  uint8_t receivedChecksum = Serial.read();
  uint8_t endByte = Serial.read();

  if (endByte != FRAME_END) {  // Malformed frame
    rejectFrame("Malformed frame - missing FRAME_END", NACK_MALFORMED_FRAME, msgId);
    return result;
  }

  uint8_t expectedChecksum = computeChecksum(msgId, length, payload);
  if (receivedChecksum != expectedChecksum) {
    sendFaultReport(F009_CHECKSUM_ERROR, SEV_WARNING);  // Existing safety/fault-log path - unchanged
    rejectFrame("Checksum mismatch on received frame", NACK_CHECKSUM_ERROR, msgId);
    return result;
  }

  result.valid = true;
  result.msgId = msgId;
  result.length = length;
  for (int i = 0; i < length; i++) result.payload[i] = payload[i];
  noteMessageReceived();
  return result;
}

void handleIncomingFrame(ReceivedFrame frame) {
  if (!frame.valid) return;

  char msg[64];
  if (frame.msgId == MSG_STATUS_UPDATE) {
    if (frame.length >= 5) {
      snprintf(msg, sizeof(msg), "RX StatusUpdate SOC=%d State=%d V=%d I=%d T=%d",
        frame.payload[0], frame.payload[1], frame.payload[2],
        (int8_t)frame.payload[3], frame.payload[4]);
    } else {
      snprintf(msg, sizeof(msg), "RX StatusUpdate SOC=%d State=%d", frame.payload[0], frame.payload[1]);
    }
    logInfo("PROTO", msg);
    sendAck(frame.msgId);
  } else if (frame.msgId == MSG_FAULT_REPORT) {
    snprintf(msg, sizeof(msg), "RX FaultReport Code=%d Severity=%d", frame.payload[0], frame.payload[1]);
    logInfo("PROTO", msg);
    if (frame.length >= 1 && frame.payload[0] == 0) {
      clearAllFaults();
    }
    sendAck(frame.msgId);
  } else if (frame.msgId == MSG_SET_SCENARIO) {
    if (frame.length >= 1) {
      setSimulationScenario(frame.payload[0]);
      sendAck(frame.msgId);
    } else {
      sendNack(NACK_MALFORMED_FRAME, frame.msgId);
    }
  } else if (frame.msgId == MSG_SET_CONFIG) {
    if (frame.length >= 5) {
      uint8_t paramId = frame.payload[0];
      float val;
      memcpy(&val, &frame.payload[1], 4);
      if (updateConfigParameter(paramId, val)) {
        sendAck(frame.msgId);
      } else {
        if (paramId >= 1 && paramId <= 8) {
          sendNack(NACK_INVALID_VALUE, frame.msgId);
        } else {
          sendNack(NACK_MALFORMED_FRAME, frame.msgId);
        }
      }
    } else {
      sendNack(NACK_MALFORMED_FRAME, frame.msgId);
    }
  } else if (frame.msgId == MSG_GET_STATUS) {
    extern BatteryData battery;
    extern SolarData solar;
    extern ChargeState currentState;
    sendStatusUpdate(battery, solar, currentState);
    sendAck(frame.msgId);
  } else if (frame.msgId == MSG_SET_MODE) {
    if (frame.length >= 1) {
      uint8_t mVal = frame.payload[0];
      extern ChargingMode manualModeOverride;
      extern bool useManualMode;
      if (mVal == 255) {
        useManualMode = false;
        sendAck(frame.msgId);
      } else if (mVal >= 0 && mVal <= 3) {
        useManualMode = true;
        manualModeOverride = (ChargingMode)mVal;
        sendAck(frame.msgId);
      } else {
        sendNack(NACK_INVALID_VALUE, frame.msgId);
      }
    } else {
      sendNack(NACK_MALFORMED_FRAME, frame.msgId);
    }
  } else if (frame.msgId == MSG_SET_FAN) {
    if (frame.length >= 1) {
      uint8_t fVal = frame.payload[0];
      extern void setFanManualOverride(bool enable, float duty);
      if (fVal == 255) {
        setFanManualOverride(false, 0.0f);
        sendAck(frame.msgId);
      } else if (fVal <= 100) {
        setFanManualOverride(true, fVal / 100.0f);
        sendAck(frame.msgId);
      } else {
        sendNack(NACK_INVALID_VALUE, frame.msgId);
      }
    } else {
      sendNack(NACK_MALFORMED_FRAME, frame.msgId);
    }
  } else if (frame.msgId == MSG_RESTART) {
    sendAck(frame.msgId);
    delay(500);
    ESP.restart();
  } else if (frame.msgId == MSG_SET_BROKER) {
    if (frame.length >= 1) {
      char broker[33];
      uint8_t len = frame.length;
      if (len > 32) len = 32;
      memcpy(broker, frame.payload, len);
      broker[len] = '\0';
      
      Preferences prefs;
      prefs.begin("wifi", false);
      prefs.putString("broker", broker);
      prefs.end();
      
      logInfo("PROTO", ("Set broker IP dynamically to: " + String(broker)).c_str());
      sendAck(frame.msgId);
      
      extern void initMQTT();
      initMQTT();
    } else {
      sendNack(NACK_MALFORMED_FRAME, frame.msgId);
    }
  } else if (frame.msgId == MSG_ACK || frame.msgId == MSG_NACK) {
    // Never ACK/NACK an ACK/NACK - a stray echo of our own reply (or the
    // host's) would otherwise ping-pong indefinitely.
    snprintf(msg, sizeof(msg), "RX %s payload[0]=%d",
      frame.msgId == MSG_ACK ? "Ack" : "Nack", frame.payload[0]);
    logInfo("PROTO", msg);
  } else {
    snprintf(msg, sizeof(msg), "RX Unknown MSG_ID=0x%02X", frame.msgId);
    logWarn("PROTO", msg);
    sendNack(NACK_UNKNOWN_MSG_ID, frame.msgId);
  }
}

// --- Timeout watchdog ---

void noteMessageReceived() {
  lastMessageTimestamp = millis();
}

bool checkCommunicationTimeout() {
  if (millis() - lastMessageTimestamp > COMM_TIMEOUT_MS) {
    return true;   // Caller decides whether to raise F008
  }
  return false;
}

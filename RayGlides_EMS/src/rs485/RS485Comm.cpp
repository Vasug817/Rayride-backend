#include "RS485Comm.h"
#include "config.h"
#include "../debug/DebugLog.h"

// Separate HardwareSerial instance for UART2, distinct from the USB
// Serial console used by RayGlidesProtocol/$serialMonitor.
HardwareSerial RS485Serial(2);

void initRS485() {
  pinMode(RS485_DE_PIN, OUTPUT);
  digitalWrite(RS485_DE_PIN, LOW);  // Start in receive mode
  RS485Serial.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  logInfo("RS485", "Initialized (9600 baud, UART2)");
}

void rs485SendFrame(uint8_t msgId, uint8_t* payload, uint8_t len) {
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  uint8_t checksum = computeChecksum(msgId, len, payload);


  digitalWrite(RS485_DE_PIN, HIGH);  // Switch transceiver to transmit
  delayMicroseconds(50);              // Small settling time before driving the bus

  RS485Serial.write(FRAME_START);
  RS485Serial.write(msgId);
  RS485Serial.write(len);
  for (int i = 0; i < len; i++) RS485Serial.write(payload[i]);
  RS485Serial.write(checksum);
  RS485Serial.write(FRAME_END);
  RS485Serial.flush();                // Wait until fully clocked out

  digitalWrite(RS485_DE_PIN, LOW);   // Release the bus back to receive mode
}

ReceivedFrame rs485ReceiveFrame() {
  ReceivedFrame result;
  result.valid = false;
  result.length = 0;

  if (RS485Serial.available() < 3) return result;
  if (RS485Serial.peek() != FRAME_START) {
    RS485Serial.read();  // Discard stray byte, resync on next call
    return result;
  }

  uint8_t start = RS485Serial.read();
  uint8_t msgId = RS485Serial.read();
  uint8_t length = RS485Serial.read();

  if (length > MAX_PAYLOAD) return result;  // Malformed - reject

  unsigned long startWait = millis();
  while ((uint8_t)RS485Serial.available() < (uint8_t)(length + 2)) {
    if (millis() - startWait > 10) {  // 10ms timeout
      return result;
    }
    delay(1);  // Wait for payload + checksum + end byte to arrive
  }

  uint8_t payload[MAX_PAYLOAD];
  for (int i = 0; i < length; i++) payload[i] = RS485Serial.read();
  uint8_t receivedChecksum = RS485Serial.read();
  uint8_t endByte = RS485Serial.read();

  if (endByte != FRAME_END) return result;

  uint8_t expectedChecksum = computeChecksum(msgId, length, payload);
  if (receivedChecksum != expectedChecksum) {
    logWarn("RS485", "Checksum mismatch on received frame");
    return result;
  }

  result.valid = true;
  result.msgId = msgId;
  result.length = length;
  for (int i = 0; i < length; i++) result.payload[i] = payload[i];
  return result;
}

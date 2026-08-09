#include "CANComm.h"
#include "config.h"
#include "driver/twai.h"

bool initCAN() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NO_ACK
  );
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();


  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    Serial.println("*** CAN: driver install failed");
    return false;
  }
  if (twai_start() != ESP_OK) {
    Serial.println("*** CAN: failed to start");
    return false;
  }
  Serial.println("CAN bus initialized (500 kbit/s)");
  return true;
}

static void transmitCANFrame(uint32_t id, uint8_t* data, uint8_t len) {
  twai_message_t message;
  message.identifier = id;
  message.extd = 0;              // Standard 11-bit ID
  message.rtr = 0;
  message.data_length_code = len;
  for (int i = 0; i < len && i < 8; i++) message.data[i] = data[i];

  esp_err_t result = twai_transmit(&message, pdMS_TO_TICKS(100));
  if (result != ESP_OK) {
    // Silenced to avoid console spam when running standalone without CAN transceivers
    // Serial.print("*** CAN TX failed for ID 0x");
    // Serial.println(id, HEX);
  }
}

void sendCANStatus(BatteryData battery, SolarData solar, ChargeState state) {
  // Payload: [SOC, ChargeState, BattV, BattI(signed), BattT, SolarV, SolarPower, BattSOH]
  // 8 bytes - exactly maxes out classic CAN's 8-byte data limit.
  uint8_t payload[8] = {
    (uint8_t)battery.soc,
    (uint8_t)state,
    (uint8_t)constrain((int)round(battery.voltage), 0, 255),
    (uint8_t)(int8_t)constrain((int)round(battery.current), -128, 127),
    (uint8_t)constrain((int)round(battery.temperature), 0, 255),
    (uint8_t)constrain((int)round(solar.voltage), 0, 255),
    (uint8_t)constrain((int)round(solar.power), 0, 255),
    (uint8_t)constrain(battery.soh, 0, 255)
  };
  transmitCANFrame(CAN_ID_STATUS, payload, 8);
}

void sendCANFault(FaultCode code, Severity sev) {
  uint8_t payload[2] = { (uint8_t)code, (uint8_t)sev };
  transmitCANFrame(CAN_ID_FAULT, payload, 2);
}

CANReceivedMessage receiveCAN() {
  CANReceivedMessage result;
  result.valid = false;
  result.length = 0;

  twai_message_t message;
  if (twai_receive(&message, 0) != ESP_OK) {
    return result;  // Nothing waiting - non-blocking
  }

  result.valid = true;
  result.id = message.identifier;
  result.length = message.data_length_code;
  for (int i = 0; i < result.length && i < 8; i++) {
    result.data[i] = message.data[i];
  }
  return result;
}

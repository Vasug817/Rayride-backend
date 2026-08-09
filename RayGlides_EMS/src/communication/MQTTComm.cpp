#include "communication/MQTTComm.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "config.h"
#include "debug/DebugLog.h"
#include "wifi/WiFiManager.h"
#include "fault/FaultManager.h"
#include "sensors/SensorSimulator.h"
#include "configuration/ConfigManager.h"
#include "communication/OfflineBuffer.h"
#include "battery/BatteryStateMachine.h"
#include "charging/ChargingDecision.h"
#include "thermal/ThermalManager.h"

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);
static unsigned long lastMQTTReconnectMs = 0;
static bool mqttConnected = false;
static uint32_t telemetrySeqNum = 0;

static String configBroker = "";
static int configPort = 1883;
static String resolvedBroker = "";

// Helper to map MQTT client states to human-readable errors
const char* getMQTTErrorString(int state) {
  switch (state) {
    case -4: return "Connection Timeout";
    case -3: return "Connection Lost";
    case -2: return "Connect Failed (Unreachable broker)";
    case -1: return "Disconnected";
    case 1:  return "Bad Protocol Version";
    case 2:  return "Identifier Rejected";
    case 3:  return "Server Unavailable";
    case 4:  return "Bad Credentials";
    case 5:  return "Unauthorized";
    default: return "Unknown Error";
  }
}

// Helper to send ACK/NACK responses over MQTT
void sendResponseMQTT(uint32_t seqNum, int msgId, const char* status, int reason = 0, const char* msg = "") {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  char deviceId[32];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "RayGlides_EMS_%02X%02X%02X", mac[3], mac[4], mac[5]);
  
  doc["device_id"] = deviceId;
  doc["acked_msg_id"] = msgId;
  doc["seq_num"] = seqNum;
  doc["status"] = status;
  if (reason != 0) {
    doc["reason"] = reason;
    doc["reason_name"] = (reason == 1) ? "CHECKSUM_ERROR" :
                         (reason == 2) ? "INVALID_MSG_ID" :
                         (reason == 3) ? "INVALID_PAYLOAD" : "UNKNOWN_ERROR";
  }
  if (strlen(msg) > 0) {
    doc["message"] = msg;
  }

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish("ems/responses", buffer);
}

// MQTT Callback when a message is received from subscribed topics
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char jsonStr[256];
  if (length >= sizeof(jsonStr)) length = sizeof(jsonStr) - 1;
  memcpy(jsonStr, payload, length);
  jsonStr[length] = '\0';

  char logMsg[300];
  snprintf(logMsg, sizeof(logMsg), "RX on topic [%s]: %s", topic, jsonStr);
  logInfo("MQTT", logMsg);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    logWarn("MQTT", "Failed to parse command JSON");
    sendResponseMQTT(0, 0, "error", 1, "Malformed JSON");
    return;
  }

  uint32_t seqNum = doc.containsKey("seq_num") ? doc["seq_num"].as<uint32_t>() : 0;

  // Verify device ID if present
  if (doc.containsKey("device_id")) {
    char deviceId[32];
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(deviceId, sizeof(deviceId), "RayGlides_EMS_%02X%02X%02X", mac[3], mac[4], mac[5]);
    const char* reqDeviceId = doc["device_id"];
    if (strcmp(reqDeviceId, "all") != 0 && strcmp(reqDeviceId, "broadcast") != 0 && strcmp(reqDeviceId, deviceId) != 0) {
      logWarn("MQTT", "Command ignored: Device ID mismatch");
      return;
    }
  }

  // Handle commands on ems/commands
  if (strcmp(topic, "ems/commands") == 0) {
    if (!doc.containsKey("msg_id")) {
      logWarn("MQTT", "Missing msg_id");
      sendResponseMQTT(seqNum, 0, "error", 3, "Missing msg_id");
      return;
    }
    
    int msgId = doc["msg_id"];
    if (msgId == 2) {
      logInfo("MQTT", "Command received: Clear Faults");
      clearAllFaults();
      sendResponseMQTT(seqNum, msgId, "success");
    } else if (msgId == 5) {
      if (doc.containsKey("payload") && doc["payload"].is<JsonArray>() && doc["payload"].size() >= 1) {
        int scenarioId = doc["payload"][0];
        if (scenarioId < 0 || scenarioId > 7) {
          logWarn("MQTT", "Invalid scenario ID");
          sendResponseMQTT(seqNum, msgId, "nack", 3, "Invalid scenario ID");
        } else {
          char msg[64];
          snprintf(msg, sizeof(msg), "Command received: Set Scenario to %d", scenarioId);
          logInfo("MQTT", msg);
          setSimulationScenario(scenarioId);
          sendResponseMQTT(seqNum, msgId, "success");
        }
      } else {
        logWarn("MQTT", "Missing or invalid payload for msg_id 5");
        sendResponseMQTT(seqNum, msgId, "nack", 3, "Missing payload");
      }
    } else if (msgId == 7) {
      logInfo("MQTT", "Command received: Get Status Polling");
      extern void runTelemetryTask();
      runTelemetryTask();
      sendResponseMQTT(seqNum, msgId, "success");
    } else if (msgId == 8) {
      if (doc.containsKey("payload") && doc["payload"].is<JsonArray>() && doc["payload"].size() >= 1) {
        int mVal = doc["payload"][0];
        extern ChargingMode manualModeOverride;
        extern bool useManualMode;
        if (mVal == 255) {
          useManualMode = false;
          logInfo("MQTT", "Command received: Restore auto charging mode");
          sendResponseMQTT(seqNum, msgId, "success");
        } else if (mVal >= 0 && mVal <= 3) {
          useManualMode = true;
          manualModeOverride = (ChargingMode)mVal;
          char msg[64];
          snprintf(msg, sizeof(msg), "Command received: Set manual charging mode to %s", modeName(manualModeOverride));
          logInfo("MQTT", msg);
          sendResponseMQTT(seqNum, msgId, "success");
        } else {
          logWarn("MQTT", "Invalid charge mode command payload");
          sendResponseMQTT(seqNum, msgId, "nack", 3, "Invalid charge mode");
        }
      } else {
        logWarn("MQTT", "Missing payload for msg_id 8");
        sendResponseMQTT(seqNum, msgId, "nack", 3, "Missing payload");
      }
    } else if (msgId == 9) {
      if (doc.containsKey("payload") && doc["payload"].is<JsonArray>() && doc["payload"].size() >= 1) {
        float dutyVal = doc["payload"][0];
        bool overrideEnable = true;
        float duty = dutyVal;
        if (dutyVal < 0.0f) {
          overrideEnable = false;
          duty = 0.0f;
        } else if (dutyVal > 1.0f) {
          duty = dutyVal / 100.0f;
        }
        
        if (overrideEnable && (duty < 0.0f || duty > 1.0f)) {
          logWarn("MQTT", "Invalid fan duty cycle override value");
          sendResponseMQTT(seqNum, msgId, "nack", 3, "Invalid fan duty value");
        } else {
          setFanManualOverride(overrideEnable, duty);
          sendResponseMQTT(seqNum, msgId, "success");
        }
      } else {
        logWarn("MQTT", "Missing payload for msg_id 9");
        sendResponseMQTT(seqNum, msgId, "nack", 3, "Missing payload");
      }
    } else if (msgId == 10) {
      logInfo("MQTT", "Command received: Remote Restarting...");
      sendResponseMQTT(seqNum, msgId, "success");
      mqttClient.disconnect();
      delay(500);
      ESP.restart();
    } else {
      logWarn("MQTT", "Unknown command msg_id received");
      sendResponseMQTT(seqNum, msgId, "nack", 2, "Unknown msg_id");
    }
  }
  // Handle config updates on ems/config
  else if (strcmp(topic, "ems/config") == 0) {
    if (doc.containsKey("param_id") && doc.containsKey("value")) {
      int paramId = doc["param_id"];
      float val = doc["value"];
      char msg[64];
      snprintf(msg, sizeof(msg), "Command received: Set Config param %d = %.2f", paramId, val);
      logInfo("MQTT", msg);
      if (updateConfigParameter(paramId, val)) {
        sendResponseMQTT(seqNum, 6, "success");
      } else {
        logWarn("MQTT", "Config validation failed");
        sendResponseMQTT(seqNum, 6, "nack", 3, "Invalid parameter value");
      }
    } else {
      logWarn("MQTT", "Config update missing param_id or value");
      sendResponseMQTT(seqNum, 6, "error", 3, "Missing config fields");
    }
  }
}

void initMQTT() {
  logInfo("MQTT", "Initializing MQTT client...");
  
  Preferences prefs;
  prefs.begin("wifi", true);
  configBroker = prefs.getString("broker", MQTT_DEFAULT_BROKER);
  configPort = prefs.getInt("port", MQTT_DEFAULT_PORT);
  prefs.end();

  resolvedBroker = configBroker;
  mqttClient.setCallback(mqttCallback);
}

void updateMQTT(unsigned long nowMs) {
  if (isWiFiConnected()) {
    if (!mqttClient.connected()) {
      mqttConnected = false;
      if (nowMs - lastMQTTReconnectMs > 5000) {
        lastMQTTReconnectMs = nowMs;

        // Dynamic mDNS resolution for broker hostname
        bool mdnsSuccess = true;
        if (configBroker.endsWith(".local")) {
          String host = configBroker.substring(0, configBroker.length() - 6);
          logInfo("MQTT", ("Querying mDNS for hostname: " + configBroker).c_str());
          
          mdnsSuccess = false;
          static bool mdnsStarted = false;
          if (!mdnsStarted) {
            if (MDNS.begin("rayglides-ems")) {
              mdnsStarted = true;
            }
          }
          
          if (mdnsStarted) {
            int n = MDNS.queryHost(host.c_str(), 2000);
            if (n > 0) {
              resolvedBroker = MDNS.IP(0).toString();
              mdnsSuccess = true;
              char msg[64];
              snprintf(msg, sizeof(msg), "mDNS Resolved to IP: %s", resolvedBroker.c_str());
              logInfo("MQTT", msg);
            } else {
              logWarn("MQTT", "mDNS query timed out!");
            }
          } else {
            logWarn("MQTT", "Failed to initialize mDNS");
          }
        }

        if (!mdnsSuccess) {
          logWarn("MQTT", "Skipping connection: Target broker hostname could not be resolved.");
        } else {
          mqttClient.setServer(resolvedBroker.c_str(), configPort);

          // Improved connection diagnostics
          char debugMsg[256];
          snprintf(debugMsg, sizeof(debugMsg), 
            "Attempting MQTT broker connection [WiFi IP: %s -> Broker: %s:%d]...",
            WiFi.localIP().toString().c_str(), resolvedBroker.c_str(), configPort);
          logInfo("MQTT", debugMsg);

          String clientId = "RayGlides_EMS_" + String(random(0xffff), HEX);
          if (mqttClient.connect(clientId.c_str())) {
            logInfo("MQTT", "MQTT connected successfully!");
            mqttConnected = true;
            mqttClient.subscribe("ems/commands");
            mqttClient.subscribe("ems/config");

            // Sync offline data
            if (hasBufferedTelemetry()) {
              uint32_t count = getBufferedCount();
              char syncStart[128];
              snprintf(syncStart, sizeof(syncStart), "{\"status\":\"sync_started\",\"count\":%d}", count);
              mqttClient.publish("ems/sync", syncStart);

              char deviceId[32];
              uint8_t mac[6];
              WiFi.macAddress(mac);
              snprintf(deviceId, sizeof(deviceId), "RayGlides_EMS_%02X%02X%02X", mac[3], mac[4], mac[5]);

              TelemetryRecord rec;
              while (getNextBufferedTelemetry(rec)) {
                StaticJsonDocument<300> doc;
                doc["device_id"] = deviceId;
                doc["sequence_num"] = rec.sequenceNum;
                doc["timestamp_offset"] = (millis() - rec.timestampMs) / 1000.0f;
                doc["soc"] = rec.soc;
                doc["state"] = stateName((ChargeState)rec.state);

                JsonObject battery = doc.createNestedObject("battery");
                battery["voltage"] = rec.battV;
                battery["current"] = rec.battI;
                battery["temp"] = rec.temp;

                JsonObject solar = doc.createNestedObject("solar");
                solar["voltage"] = rec.solarV;
                solar["power"] = rec.solarP;

                char buffer[300];
                serializeJson(doc, buffer);
                mqttClient.publish("ems/historical", buffer);

                popBufferedTelemetry();
                delay(30); // Prevent buffer starvation or flooding
              }

              mqttClient.publish("ems/sync", "{\"status\":\"sync_completed\"}");
              logInfo("MQTT", "Offline telemetry buffer successfully synchronized.");
            }
          } else {
            int state = mqttClient.state();
            char err[128];
            snprintf(err, sizeof(err), "MQTT connection failed! Reason: %s (state=%d)", 
              getMQTTErrorString(state), state);
            logWarn("MQTT", err);
          }
        }
      }
    } else {
      mqttConnected = true;
      mqttClient.loop();
    }
  } else {
    mqttConnected = false;
  }
}

bool isMQTTConnected() {
  return mqttConnected;
}

void publishTelemetryJSON(
  float battV, float battI, float temp, int soc, int soh, 
  float solarV, float solarP, float fanDuty, 
  float solarWh, float chgWh, float consWh, float netAh, 
  uint8_t state, uint8_t mode, const char* fault
) {
  uint32_t currentSeq = telemetrySeqNum++;

  if (!isMQTTConnected()) {
    bufferTelemetry(currentSeq, (uint8_t)soc, state, battV, battI, temp, solarV, solarP);
    return;
  }

  StaticJsonDocument<512> doc;
  char deviceId[32];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "RayGlides_EMS_%02X%02X%02X", mac[3], mac[4], mac[5]);

  doc["device_id"] = deviceId;
  doc["timestamp"] = millis();
  doc["sequence_num"] = currentSeq;
  
  doc["soc"] = soc;
  doc["soh"] = soh;
  doc["state"] = stateName((ChargeState)state);
  doc["mode"] = modeName((ChargingMode)mode);
  doc["fault"] = fault;
  
  JsonObject battery = doc.createNestedObject("battery");
  battery["voltage"] = battV;
  battery["current"] = battI;
  battery["temp"] = temp;
  
  JsonObject solar = doc.createNestedObject("solar");
  solar["voltage"] = solarV;
  solar["power"] = solarP;
  
  JsonObject cooling = doc.createNestedObject("cooling");
  cooling["fan_duty"] = fanDuty;
  
  JsonObject energy = doc.createNestedObject("energy");
  energy["solar_wh"] = solarWh;
  energy["charge_wh"] = chgWh;
  energy["consumed_wh"] = consWh;
  energy["net_ah"] = netAh;

  char buffer[512];
  serializeJson(doc, buffer);
  mqttClient.publish("ems/telemetry", buffer);
}

void publishFaultJSON(int faultCode, const char* faultName, int severity) {
  if (!isMQTTConnected()) return;

  StaticJsonDocument<128> doc;
  doc["fault_code"] = faultCode;
  doc["fault_name"] = faultName;
  doc["severity"] = (severity == SEV_CRITICAL) ? "CRITICAL" : "WARNING";

  char buffer[128];
  serializeJson(doc, buffer);
  mqttClient.publish("ems/faults", buffer);
}

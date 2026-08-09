#include "WiFiManager.h"
#include <WiFi.h>
#include <Preferences.h>
#include "../debug/DebugLog.h"
#include "config.h"

static Preferences prefs;
static bool connected = false;
static bool apModeActive = false;
static unsigned long lastConnectionAttemptMs = 0;
static const unsigned long connectionTimeoutMs = 10000; // 10 seconds

void initWiFi() {
  logInfo("WIFI", "Initializing Wi-Fi Manager (AP+STA mode)...");
  
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  
  prefs.begin("wifi", false);
  String ssid = "";
  String pass = "";
  if (prefs.isKey("ssid")) ssid = prefs.getString("ssid");
  if (prefs.isKey("pass")) pass = prefs.getString("pass");
  prefs.end();

  if (ssid.length() == 0) {
    ssid = WIFI_DEFAULT_SSID;
    pass = WIFI_DEFAULT_PASS;
  }

  // Always start AP mode so it is available for local rescue/OTA
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("RayGlides_EMS_AP", "12345678");
  logInfo("WIFI", "Access Point started: 'RayGlides_EMS_AP' (password: 12345678)");

  if (ssid.length() > 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to SSID: %s", ssid.c_str());
    logInfo("WIFI", msg);
    WiFi.begin(ssid.c_str(), pass.c_str());
    lastConnectionAttemptMs = millis();
    connected = false;
  } else {
    logWarn("WIFI", "No stored or default STA credentials.");
    connected = false;
  }
}

void startAPMode() {
  // Already in AP_STA mode, nothing to do
}

void saveWiFiCredentials(const char* ssid, const char* password) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
  
  char msg[64];
  snprintf(msg, sizeof(msg), "Credentials saved for: %s. Restarting Wi-Fi...", ssid);
  logInfo("WIFI", msg);
  
  initWiFi();
}

bool isWiFiConnected() {
  return connected;
}

void updateWiFi() {
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (!connected) {
      connected = true;
      char msg[64];
      snprintf(msg, sizeof(msg), "Connected! IP address: %s", WiFi.localIP().toString().c_str());
      logInfo("WIFI", msg);
    }
  } else {
    if (connected) {
      logWarn("WIFI", "Connection lost!");
      connected = false;
      lastConnectionAttemptMs = millis();
    }
    
    if (millis() - lastConnectionAttemptMs > 15000) {
      logInfo("WIFI", "Retrying connection to SSID...");
      
      prefs.begin("wifi", true); // Read-only to avoid NVS driver warn logging
      String ssid = prefs.getString("ssid", WIFI_DEFAULT_SSID);
      String pass = prefs.getString("pass", WIFI_DEFAULT_PASS);
      prefs.end();
      
      if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
      }
      lastConnectionAttemptMs = millis();
    }
  }
}

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

void initWiFi();
void updateWiFi();
bool isWiFiConnected();
void saveWiFiCredentials(const char* ssid, const char* password);
void startAPMode();

#endif

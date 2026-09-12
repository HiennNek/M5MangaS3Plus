#pragma once

#include <Arduino.h>

void startWifiServer();
void stopWifiServer();
void updateWifiServer();
bool isWifiServerRunning();
String getWifiIP();
String getWifiSSID();

#pragma once

#include <string>

void startWifiServer();
void stopWifiServer();
void updateWifiServer();
bool isWifiServerRunning();
std::string getWifiIP();
std::string getWifiSSID();

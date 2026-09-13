#pragma once

#include <string>

void startWifiServer();
void stopWifiServer();
void updateWifiServer(); // No-op under IDF (httpd runs its own task)
bool isWifiServerRunning();
std::string getWifiIP();
std::string getWifiSSID();

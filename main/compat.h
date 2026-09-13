#pragma once
// Small compatibility layer replacing the Arduino helpers used by the
// original sketch (millis/delay/String utils/setCpuFrequencyMhz).

#include <cstdint>
#include <string>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- Time -----------------------------------------------------------------
inline int64_t now_ms() { return esp_timer_get_time() / 1000; }
inline int64_t now_us() { return esp_timer_get_time(); }
inline void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// --- Strings (std::string replacements for Arduino String helpers) --------
inline bool str_starts_with(const std::string &s, const std::string &prefix)
{
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

inline bool str_starts_with(const std::string &s, char c)
{
  return !s.empty() && s.front() == c;
}

inline bool str_ends_with(const std::string &s, const std::string &suffix)
{
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string str_trim(const std::string &s)
{
  const char *ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

// Basename of a VFS path (text after the last '/').
inline std::string path_basename(const std::string &path)
{
  size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return path;
  return path.substr(slash + 1);
}

// --- CPU frequency hints ---------------------------------------------------
// Original code called setCpuFrequencyMhz(240) while rendering and
// setCpuFrequencyMhz(80) while idle. Under IDF, DFS (see sdkconfig.defaults)
// scales automatically; these hold/release a PM lock to bias to max while
// rendering. Safe to call redundantly; all call sites run on the main task.
void cpu_init_power_management();
void cpu_high_performance(); // ~ setCpuFrequencyMhz(240)
void cpu_power_save();       // ~ setCpuFrequencyMhz(80)

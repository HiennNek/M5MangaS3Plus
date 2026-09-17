#pragma once
// Small Arduino -> ESP-IDF compatibility helpers for the M5MangaS3Plus port.
// M5Unified/M5GFX come from the ESP Component Registry (JPEGDEC is vendored
// under components/jpegdec); everything else uses native IDF APIs
// (FreeRTOS, esp_timer, NVS, SDMMC, esp_http_server).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef PROGMEM
#define PROGMEM
#endif

// ---- timing (Arduino: millis/micros/delay) ------------------------------
inline uint32_t idf_millis() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
inline uint32_t idf_micros() {
  return (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);
}
inline void idf_delay(uint32_t ms) {
  if (ms == 0) {
    taskYIELD();
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(ms));
}

// ---- CPU frequency --------------------------------------------------------
// Arduino setCpuFrequencyMhz() does not exist in ESP-IDF. Clock scaling is
// handled by CONFIG_PM_ENABLE + automatic DFS, so these become no-ops that
// keep the original call sites compiling while preserving intent in comments.
inline void setCpuFrequencyMhz(uint32_t /*mhz*/) {}

// ---- random ---------------------------------------------------------------
inline long idf_random(long max) {
  if (max <= 0) return 0;
  return (long)(esp_random() % (uint32_t)max);
}

// ---- std::string helpers (Arduino String idioms) --------------------------
inline bool str_starts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}
inline bool str_starts_with(const std::string &s, const char *prefix) {
  return str_starts_with(s, std::string(prefix));
}
inline bool str_starts_with(const std::string &s, char c) {
  return !s.empty() && s[0] == c;
}
inline bool str_ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
inline bool str_ends_with(const std::string &s, const char *suffix) {
  return str_ends_with(s, std::string(suffix));
}
inline void str_trim(std::string &s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}
inline std::string str_substring(const std::string &s, size_t from,
                                 size_t to = std::string::npos) {
  if (from >= s.size()) return std::string();
  if (to == std::string::npos || to > s.size()) to = s.size();
  if (to <= from) return std::string();
  return s.substr(from, to - from);
}

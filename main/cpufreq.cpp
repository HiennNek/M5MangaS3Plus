#include "cpufreq.h"

#include "esp_pm.h"
#include "freertos/FreeRTOS.h"

static esp_pm_lock_handle_t s_lock = nullptr;
static bool s_latched = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Created on first use: app_main runs before esp_pm is necessarily settled,
// and a failed create just degrades to the old (no-op) behaviour.
static esp_pm_lock_handle_t lockHandle() {
  if (!s_lock) {
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "manga", &s_lock) != ESP_OK)
      s_lock = nullptr;
  }
  return s_lock;
}

// Latching: repeated cpuSetFast(true) does not stack, so the existing
// unbalanced call sites in main.cpp/ui.cpp stay correct.
void cpuSetFast(bool fast) {
  esp_pm_lock_handle_t h = lockHandle();
  if (!h) return;

  bool change = false;
  portENTER_CRITICAL(&s_mux);
  if (fast != s_latched) {
    s_latched = fast;
    change = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!change) return;

  // Must be outside the critical section: these take a mutex internally.
  if (fast)
    esp_pm_lock_acquire(h);
  else
    esp_pm_lock_release(h);
}

CpuBoost::CpuBoost() {
  if (esp_pm_lock_handle_t h = lockHandle()) esp_pm_lock_acquire(h);
}

CpuBoost::~CpuBoost() {
  if (s_lock) esp_pm_lock_release(s_lock);
}

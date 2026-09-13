#include "compat.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "esp_log.h"

static const char *TAG = "manga_power";

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_perf_lock = nullptr;
static bool s_perf_held = false;
#endif

void cpu_init_power_management()
{
#ifdef CONFIG_PM_ENABLE
  esp_pm_config_t pm_cfg = {};
  pm_cfg.max_freq_mhz = 240;
  pm_cfg.min_freq_mhz = 80;
  pm_cfg.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&pm_cfg);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
    return;
  }
  if (s_perf_lock == nullptr)
  {
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "render", &s_perf_lock);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "esp_pm_lock_create failed: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGI(TAG, "PM not enabled, running at default frequency");
#endif
}

void cpu_high_performance()
{
#ifdef CONFIG_PM_ENABLE
  if (s_perf_lock && !s_perf_held)
  {
    esp_pm_lock_acquire(s_perf_lock);
    s_perf_held = true;
  }
#endif
}

void cpu_power_save()
{
#ifdef CONFIG_PM_ENABLE
  if (s_perf_lock && s_perf_held)
  {
    esp_pm_lock_release(s_perf_lock);
    s_perf_held = false;
  }
#endif
}

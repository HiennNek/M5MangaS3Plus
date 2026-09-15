#include <string>

#include "M5Unified.h"
#include "bookmarks.h"
#include "compat.h"
#include "config.h"
#include "esp_log.h"
#include "input.h"
#include "navigation.h"
#include "nvs_flash.h"
#include "state.h"
#include "storage.h"
#include "ui.h"
#include "wifi_server.h"

static const char *TAG = "main";
static uint32_t lastInteractionMs = 0;

void openManga(int idx) {
  if (idx < 0 || idx >= (int)mangaFolders.size()) return;
  std::string path = std::string(MANGA_ROOT) + "/" + mangaFolders[idx];
  openMangaPath(path, 0);
}

void openMangaPath(const std::string &path, int page) {
  currentMangaPath = path;
  currentPage = page;

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(20, DISPLAY_H / 2 - 20);
  M5.Display.print("Opening...");
  M5.Display.display();

  setCpuFrequencyMhz(240);
  uint32_t t0 = idf_millis();
  totalPages = findTotalPages(currentMangaPath);
  ESP_LOGI(TAG, "Found %d pages in %lu ms", totalPages,
           (unsigned long)(idf_millis() - t0));

  if (totalPages == 0) {
    if (isCbzPath(currentMangaPath))
      drawError("No images found in archive.");
    else
      drawError("No images found.\nExpected: m5_0000.jpg ...");
    idf_delay(2500);
    currentEpdMode = epd_mode_t::epd_fast;
    needRedraw = true;
    return;
  }

  saveProgress();
  setCpuFrequencyMhz(80);

  currentEpdMode = epd_mode_t::epd_quality;
  appState = STATE_READER;
  needRedraw = true;
}

extern "C" void app_main(void) {
  // Persistent storage for reading progress (replaces Arduino Preferences).
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  M5.begin(cfg);

  // CPU runs at 240MHz by default; CONFIG_PM_ENABLE handles scaling.
  // (Arduino setCpuFrequencyMhz() calls are no-ops in this port.)
  setCpuFrequencyMhz(240);

  // No seed needed: esp_random() is a true RNG.

  currentEpdMode = epd_mode_t::epd_fast;
  M5.Display.setRotation(0);
  M5.Display.setColorDepth(8);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);

  sdInit();
  scanMangaFolders();
  loadProgress();
  loadBookmarks();

  needRedraw = true;
  lastInteractionMs = idf_millis();

  while (true) {
    M5.update();
    if (M5.Touch.getCount() > 0) {
      lastInteractionMs = idf_millis();
    }

    if (appState == STATE_WIFI) {
      updateWifiServer();
    }

    if (needRedraw) {
      setCpuFrequencyMhz(240);
      M5.Display.setEpdMode(currentEpdMode);
      if (controlMenuOpen)
        drawControlCenter();
      else if (bookConfigOpen)
        drawBookConfig();
      else if (appState == STATE_MENU)
        drawMenu();
      else if (appState == STATE_BOOKMARKS)
        drawBookmarks();
      else if (appState == STATE_WIFI)
        drawWifiServer();
      else
        drawPage();
      needRedraw = false;
    }

    handleTouch();

    bool readerIdle =
        appState == STATE_READER && !controlMenuOpen && !bookConfigOpen;
    bool wifiActive = appState == STATE_WIFI;

    if (wifiActive) {
      setCpuFrequencyMhz(240);
      idf_delay(1);
    } else if (readerIdle) {
      if (idf_millis() - lastInteractionMs >= 5000) {
        setCpuFrequencyMhz(80);
        // M5.update() already pumps background tasks; yield briefly.
        vTaskDelay(pdMS_TO_TICKS(50));
      } else {
        setCpuFrequencyMhz(240);
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      setCpuFrequencyMhz(240);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

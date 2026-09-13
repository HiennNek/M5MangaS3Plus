#include "bookmarks.h"
#include "compat.h"
#include "config.h"
#include "input.h"
#include "navigation.h"
#include "state.h"
#include "storage.h"
#include "ui.h"
#include "wifi_server.h"
#include <M5Unified.h>

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "manga_main";

static int64_t lastInteractionMs = 0;

extern "C" void app_main(void)
{
  // NVS (reading progress) must come up before storage/wifi use it.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  cpu_init_power_management();

  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  M5.begin(cfg);

  cpu_high_performance();

  ESP_LOGI(TAG, "M5MangaS3+ (ESP-IDF) starting");

  currentEpdMode = epd_mode_t::epd_fast;
  M5.Display.setRotation(0);
  M5.Display.setColorDepth(8);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);

  // Skip the redundant "Initialising..." screen refresh to save ~1s of E-ink
  // update time. The first real frame will be drawn in the loop below.

  sdInit();
  scanMangaFolders();
  loadProgress();
  loadBookmarks();

  needRedraw = true;
  lastInteractionMs = now_ms();

  while (1)
  {
    M5.update();
    if (M5.Touch.getCount() > 0)
    {
      lastInteractionMs = now_ms();
    }

    if (appState == STATE_WIFI)
    {
      updateWifiServer();
    }

    if (needRedraw)
    {
      cpu_high_performance();
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

    if (wifiActive)
    {
      cpu_high_performance();
      delay_ms(1);
    }
    else if (readerIdle)
    {
      if (now_ms() - lastInteractionMs >= 5000)
      {
        cpu_power_save();
        delay_ms(50);
      }
      else
      {
        cpu_high_performance();
        delay_ms(10);
      }
    }
    else
    {
      cpu_high_performance();
      delay_ms(10);
    }
  }
}

void openManga(int idx)
{
  if (idx < 0 || idx >= (int)mangaFolders.size())
    return;
  std::string path = std::string(MANGA_ROOT) + "/" + mangaFolders[idx];
  openMangaPath(path, 0);
}

void openMangaPath(const std::string &path, int page)
{
  currentMangaPath = path;
  currentPage = page;

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(20, DISPLAY_H / 2 - 20);
  M5.Display.print("Opening...");
  M5.Display.display();

  cpu_high_performance();
  int64_t t0 = now_ms();
  totalPages = findTotalPages(currentMangaPath);
  ESP_LOGI(TAG, "Binary search found %d pages in %lld ms", totalPages,
           (long long)(now_ms() - t0));

  if (totalPages == 0)
  {
    drawError("No images found.\nExpected: m5_0000.jpg ...");
    delay_ms(2500);
    currentEpdMode = epd_mode_t::epd_fast;
    needRedraw = true;
    return;
  }

  saveProgress();
  cpu_power_save();

  currentEpdMode = epd_mode_t::epd_quality;
  appState = STATE_READER;
  needRedraw = true;
}

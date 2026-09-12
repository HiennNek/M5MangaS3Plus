#include "bookmarks.h"
#include "config.h"
#include "input.h"
#include "navigation.h"
#include "state.h"
#include "storage.h"
#include "ui.h"
#include "wifi_server.h"
#include <Arduino.h>
#include <M5Unified.h>

static unsigned long lastInteractionMs = 0;

void setup()
{
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  M5.begin(cfg);

  // Boost CPU for fast initialization
  setCpuFrequencyMhz(240);

  Serial.begin(115200);

  randomSeed(micros());

  currentEpdMode = epd_mode_t::epd_fast;
  M5.Display.setRotation(0);
  M5.Display.setColorDepth(8);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);

  // Skip the redundant "Initialising..." screen refresh to save ~1s of E-ink
  // update time. The first real frame will be drawn in loop().

  sdInit();
  scanMangaFolders();
  loadProgress();
  loadBookmarks();

  needRedraw = true;
}

void loop()
{
  M5.update();
  if (M5.Touch.getCount() > 0)
  {
    lastInteractionMs = millis();
  }

  if (appState == STATE_WIFI)
  {
    updateWifiServer();
  }

  if (needRedraw)
  {
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

  if (wifiActive)
  {
    setCpuFrequencyMhz(240);
    delay(1);
  }
  else if (readerIdle)
  {
    if (millis() - lastInteractionMs >= 5000)
    {
      setCpuFrequencyMhz(80);
      M5.delay(50); // M5.delay handles background tasks better
    }
    else
    {
      setCpuFrequencyMhz(240);
      M5.delay(10);
    }
  }
  else
  {
    setCpuFrequencyMhz(240);
    M5.delay(10);
  }
}

void openManga(int idx)
{
  if (idx < 0 || idx >= (int)mangaFolders.size())
    return;
  String path = String(MANGA_ROOT) + "/" + mangaFolders[idx];
  openMangaPath(path, 0);
}

void openMangaPath(const String &path, int page)
{
  currentMangaPath = path;
  currentPage = page;

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(20, DISPLAY_H / 2 - 20);
  M5.Display.print("Opening...");
  M5.Display.display();

  setCpuFrequencyMhz(240);
  unsigned long t0 = millis();
  totalPages = findTotalPages(currentMangaPath);
  Serial.printf("Binary search found %d pages in %lu ms\n", totalPages,
                millis() - t0);

  if (totalPages == 0)
  {
    drawError("No images found.\nExpected: m5_0000.jpg ...");
    delay(2500);
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

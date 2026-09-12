#include "storage.h"
#include "ui.h"
#include <M5Unified.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>

void sdInit()
{
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  int retry = 0;
  // Boost SD SPI to 80MHz for maximum performance with UHS-I cards
  while (!SD.begin(SD_CS_PIN, SPI, 80000000))
  {
    if (retry == 0)
    {
      M5.Display.fillScreen(TFT_WHITE);
      M5.Display.setCursor(20, 20);
    }
    M5.Display.printf("SD init failed (attempt %d)...\n", ++retry);
    M5.Display.display();
    delay(1000);
    if (retry >= 5)
    {
      drawError("SD card not found!\nInsert SD and reset.");
      while (1)
        delay(1000);
    }
  }
  Serial.println("SD OK");
}

void scanMangaFolders()
{
  setCpuFrequencyMhz(240);
  mangaFolders.clear();
  mangaPageCounts.clear();

  File root = SD.open(MANGA_ROOT);
  if (!root || !root.isDirectory())
  {
    Serial.println("No /manga directory");
    return;
  }

  File entry;
  while ((entry = root.openNextFile()))
  {
    if (entry.isDirectory())
    {
      String name = entry.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0)
        name = name.substring(slash + 1);
      if (name.length() > 0 && !name.startsWith("."))
      {
        mangaFolders.push_back(name);
        Serial.printf("  Folder: %s\n", name.c_str());
      }
    }
    entry.close();
  }
  root.close();

  std::sort(mangaFolders.begin(), mangaFolders.end());
  mangaPageCounts.assign(mangaFolders.size(), -1);
  Serial.printf("Found %d manga folders\n", (int)mangaFolders.size());
  setCpuFrequencyMhz(80);
}

static int findCachedPageCount(const String &folder)
{
  int slash = folder.lastIndexOf('/');
  String name = (slash >= 0) ? folder.substring(slash + 1) : folder;
  for (size_t i = 0; i < mangaFolders.size(); ++i)
  {
    if (mangaFolders[i] == name)
    {
      return mangaPageCounts[i];
    }
  }
  return -1;
}

static void storeCachedPageCount(const String &folder, int count)
{
  int slash = folder.lastIndexOf('/');
  String name = (slash >= 0) ? folder.substring(slash + 1) : folder;
  for (size_t i = 0; i < mangaFolders.size(); ++i)
  {
    if (mangaFolders[i] == name)
    {
      mangaPageCounts[i] = count;
      return;
    }
  }
}

String makePagePath(const String &folder, int n)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "%s/%s%0*d%s", folder.c_str(), IMG_PREFIX,
           IMG_DIGITS, n, IMG_SUFFIX);
  return String(buf);
}

static bool pageExistsFast(char *buf, int prefixLen, int n)
{
  sprintf(buf + prefixLen, "/%s%0*d%s", IMG_PREFIX, IMG_DIGITS, n, IMG_SUFFIX);
  return SD.exists(buf);
}

int findTotalPages(const String &folder)
{
  static String cachedFolder = "";
  static int cachedCount = 0;
  if (folder == cachedFolder)
    return cachedCount;

  int count = findCachedPageCount(folder);
  if (count >= 0)
  {
    cachedFolder = folder;
    cachedCount = count;
    return count;
  }

  char pathBuf[256];
  strncpy(pathBuf, folder.c_str(), sizeof(pathBuf) - 64);
  pathBuf[sizeof(pathBuf) - 65] = '\0';
  int prefixLen = strlen(pathBuf);

  if (!pageExistsFast(pathBuf, prefixLen, 0))
  {
    cachedFolder = folder;
    cachedCount = 0;
    storeCachedPageCount(folder, 0);
    return 0;
  }

  int hi = 1;
  while (pageExistsFast(pathBuf, prefixLen, hi))
  {
    hi *= 2;
    if (hi > 100000)
    {
      hi = 100000;
      break;
    }
  }

  int lo = hi / 2;
  while (lo + 1 < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (pageExistsFast(pathBuf, prefixLen, mid))
      lo = mid;
    else
      hi = mid;
  }

  cachedFolder = folder;
  cachedCount = lo + 1;
  storeCachedPageCount(folder, cachedCount);
  return cachedCount;
}

void saveProgress()
{
  static unsigned long lastSaveMs = 0;
  unsigned long now = millis();
  lastMangaPath = currentMangaPath;
  lastPage = currentPage;
  updateLastMangaName();
  if (now - lastSaveMs < 2000)
    return;
  lastSaveMs = now;
  prefs.begin("manga", false);
  prefs.putString("lastPath", currentMangaPath);
  prefs.putInt("lastPage", currentPage);
  prefs.end();
}

void loadProgress()
{
  prefs.begin("manga", true);
  lastMangaPath = prefs.getString("lastPath", "");
  lastPage = prefs.getInt("lastPage", 0);
  prefs.end();
  updateLastMangaName();
}

void updateLastMangaName()
{
  if (lastMangaPath.length() == 0)
  {
    lastMangaName = "";
    return;
  }
  int slash = lastMangaPath.lastIndexOf('/');
  if (slash >= 0)
    lastMangaName = lastMangaPath.substring(slash + 1);
  else
    lastMangaName = lastMangaPath;
}

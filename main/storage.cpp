#include "storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "compat.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "ui.h"
#include <M5Unified.h>

static const char *TAG = "manga_storage";

#define MOUNT_POINT "/sdcard"

void sdInit()
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 8;
  mount_config.allocation_unit_size = 16 * 1024;
  mount_config.disk_status_check_enable = false;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = SD_MOSI_PIN;
  bus_cfg.miso_io_num = SD_MISO_PIN;
  bus_cfg.sclk_io_num = SD_SCK_PIN;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4000;
  esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg,
                                     SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SD_CS_PIN;
  slot_config.host_id = (spi_host_device_t)host.slot;

  // The Arduino build overclocked the SD bus to 80MHz. Try speeds
  // fastest-first and fall back until the card mounts.
  static const uint32_t speeds_khz[] = {80000, 40000, SDMMC_FREQ_DEFAULT};
  sdmmc_card_t *card = nullptr;
  bool mounted = false;
  for (uint32_t khz : speeds_khz)
  {
    host.max_freq_khz = khz;
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setCursor(20, 20);
    M5.Display.printf("Mounting SD at %lu MHz...\n", (unsigned long)(khz / 1000));
    M5.Display.display();
    if (esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                &mount_config, &card) == ESP_OK)
    {
      mounted = true;
      ESP_LOGI(TAG, "SD mounted at " MOUNT_POINT " (%lu kHz)",
               (unsigned long)khz);
      break;
    }
    ESP_LOGW(TAG, "SD mount failed at %lu kHz, trying slower",
             (unsigned long)khz);
    delay_ms(300);
  }
  if (!mounted)
  {
    drawError("SD card not found!\nInsert SD and reset.");
    while (1)
      delay_ms(1000);
  }
}

void scanMangaFolders()
{
  cpu_high_performance();
  mangaFolders.clear();
  mangaPageCounts.clear();

  DIR *dir = opendir(MANGA_ROOT);
  if (!dir)
  {
    ESP_LOGW(TAG, "No %s directory", MANGA_ROOT);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr)
  {
    std::string name = entry->d_name;
    if (name.empty() || name[0] == '.')
      continue;
    // readdir lists files too - keep directories only.
    std::string full = std::string(MANGA_ROOT) + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    {
      mangaFolders.push_back(name);
      ESP_LOGI(TAG, "  Folder: %s", name.c_str());
    }
  }
  closedir(dir);

  std::sort(mangaFolders.begin(), mangaFolders.end());
  mangaPageCounts.assign(mangaFolders.size(), -1);
  ESP_LOGI(TAG, "Found %d manga folders", (int)mangaFolders.size());
  cpu_power_save();
}

static int findCachedPageCount(const std::string &folder)
{
  std::string name = path_basename(folder);
  for (size_t i = 0; i < mangaFolders.size(); ++i)
  {
    if (mangaFolders[i] == name)
      return mangaPageCounts[i];
  }
  return -1;
}

static void storeCachedPageCount(const std::string &folder, int count)
{
  std::string name = path_basename(folder);
  for (size_t i = 0; i < mangaFolders.size(); ++i)
  {
    if (mangaFolders[i] == name)
    {
      mangaPageCounts[i] = count;
      return;
    }
  }
}

std::string makePagePath(const std::string &folder, int n)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "%s/%s%0*d%s", folder.c_str(), IMG_PREFIX,
           IMG_DIGITS, n, IMG_SUFFIX);
  return std::string(buf);
}

static bool pathExists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static bool pageExistsFast(char *buf, int prefixLen, int n)
{
  snprintf(buf + prefixLen, 256 - prefixLen, "/%s%0*d%s", IMG_PREFIX,
           IMG_DIGITS, n, IMG_SUFFIX);
  return pathExists(buf);
}

int findTotalPages(const std::string &folder)
{
  static std::string cachedFolder = "";
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
  static int64_t lastSaveMs = 0;
  int64_t now = now_ms();
  lastMangaPath = currentMangaPath;
  lastPage = currentPage;
  updateLastMangaName();
  if (now - lastSaveMs < 2000)
    return;
  lastSaveMs = now;

  nvs_handle_t h;
  if (nvs_open("manga", NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_str(h, "lastPath", currentMangaPath.c_str());
  nvs_set_i32(h, "lastPage", (int32_t)currentPage);
  nvs_commit(h);
  nvs_close(h);
}

void loadProgress()
{
  nvs_handle_t h;
  if (nvs_open("manga", NVS_READONLY, &h) != ESP_OK)
  {
    updateLastMangaName();
    return;
  }
  char path[256] = {0};
  size_t len = sizeof(path);
  int32_t page = 0;
  if (nvs_get_str(h, "lastPath", path, &len) == ESP_OK)
    lastMangaPath = path;
  if (nvs_get_i32(h, "lastPage", &page) == ESP_OK)
    lastPage = (int)page;
  nvs_close(h);
  updateLastMangaName();
}

void updateLastMangaName()
{
  if (lastMangaPath.empty())
  {
    lastMangaName = "";
    return;
  }
  lastMangaName = path_basename(lastMangaPath);
}

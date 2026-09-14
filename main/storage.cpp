#include "storage.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "compat.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "ui.h"
#include "driver/spi_master.h"

static const char *TAG = "storage";
static sdmmc_card_t *s_card = nullptr;

void sdInit() {
  ESP_LOGI(TAG, "Initializing SD card over SPI");
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 8,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = (gpio_num_t)SD_MOSI_PIN;
  bus_cfg.miso_io_num = (gpio_num_t)SD_MISO_PIN;
  bus_cfg.sclk_io_num = (gpio_num_t)SD_SCK_PIN;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4000;

  // SPI2_HOST is free on ESP32-S3 (SPI3 used by display via M5Unified).
  esp_err_t ret =
      spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  // Boost SD SPI for UHS-I cards (Arduino used 80MHz).
  host.max_freq_khz = 80000;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SD_CS_PIN;
  slot_config.host_id = (spi_host_device_t)SPI2_HOST;

  int retry = 0;
  while (esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                 &mount_config, &s_card) != ESP_OK) {
    ESP_LOGW(TAG, "SD init failed (attempt %d)...", ++retry);
    idf_delay(1000);
    if (retry >= 5) {
      drawError("SD card not found!\nInsert SD and reset.");
      while (1) idf_delay(1000);
    }
  }
  ESP_LOGI(TAG, "SD OK");
}

void scanMangaFolders() {
  setCpuFrequencyMhz(240);
  mangaFolders.clear();
  mangaPageCounts.clear();

  DIR *dir = opendir(MANGA_ROOT);
  if (!dir) {
    ESP_LOGW(TAG, "No %s directory", MANGA_ROOT);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name.empty() || name[0] == '.') continue;
    // Strip any leading path (readdir gives bare names, but keep parity).
    size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty() || name[0] == '.') continue;
    // Confirm it is a directory.
    struct stat st = {};
    std::string full = std::string(MANGA_ROOT) + "/" + name;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      mangaFolders.push_back(name);
      ESP_LOGI(TAG, "  Folder: %s", name.c_str());
    }
  }
  closedir(dir);

  std::sort(mangaFolders.begin(), mangaFolders.end());
  mangaPageCounts.assign(mangaFolders.size(), -1);
  ESP_LOGI(TAG, "Found %d manga folders", (int)mangaFolders.size());
  setCpuFrequencyMhz(80);
}

static int findCachedPageCount(const std::string &folder) {
  size_t slash = folder.rfind('/');
  std::string name =
      (slash != std::string::npos) ? folder.substr(slash + 1) : folder;
  for (size_t i = 0; i < mangaFolders.size(); ++i) {
    if (mangaFolders[i] == name) {
      return mangaPageCounts[i];
    }
  }
  return -1;
}

static void storeCachedPageCount(const std::string &folder, int count) {
  size_t slash = folder.rfind('/');
  std::string name =
      (slash != std::string::npos) ? folder.substr(slash + 1) : folder;
  for (size_t i = 0; i < mangaFolders.size(); ++i) {
    if (mangaFolders[i] == name) {
      mangaPageCounts[i] = count;
      return;
    }
  }
}

std::string makePagePath(const std::string &folder, int n) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s/%s%0*d%s", folder.c_str(), IMG_PREFIX,
           IMG_DIGITS, n, IMG_SUFFIX);
  return std::string(buf);
}

static bool pageExistsFast(char *buf, int prefixLen, int n) {
  snprintf(buf + prefixLen, 256 - (size_t)prefixLen, "/%s%0*d%s", IMG_PREFIX,
           IMG_DIGITS, n, IMG_SUFFIX);
  return access(buf, F_OK) == 0;
}

bool pageExists(const std::string &folder, int n) {
  return access(makePagePath(folder, n).c_str(), F_OK) == 0;
}

int findTotalPages(const std::string &folder) {
  static std::string cachedFolder = "";
  static int cachedCount = 0;
  if (folder == cachedFolder) return cachedCount;

  int count = findCachedPageCount(folder);
  if (count >= 0) {
    cachedFolder = folder;
    cachedCount = count;
    return count;
  }

  char pathBuf[256];
  strncpy(pathBuf, folder.c_str(), sizeof(pathBuf) - 64);
  pathBuf[sizeof(pathBuf) - 65] = '\0';
  int prefixLen = strlen(pathBuf);

  if (!pageExistsFast(pathBuf, prefixLen, 0)) {
    cachedFolder = folder;
    cachedCount = 0;
    storeCachedPageCount(folder, 0);
    return 0;
  }

  int hi = 1;
  while (pageExistsFast(pathBuf, prefixLen, hi)) {
    hi *= 2;
    if (hi > 100000) {
      hi = 100000;
      break;
    }
  }

  int lo = hi / 2;
  while (lo + 1 < hi) {
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

void saveProgress() {
  static uint32_t lastSaveMs = 0;
  uint32_t now = idf_millis();
  lastMangaPath = currentMangaPath;
  lastPage = currentPage;
  updateLastMangaName();
  if (now - lastSaveMs < 2000) return;
  lastSaveMs = now;
  nvs_handle_t h;
  if (nvs_open("manga", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "lastPath", currentMangaPath.c_str());
  nvs_set_i32(h, "lastPage", (int32_t)currentPage);
  nvs_commit(h);
  nvs_close(h);
}

void loadProgress() {
  nvs_handle_t h;
  if (nvs_open("manga", NVS_READONLY, &h) != ESP_OK) {
    updateLastMangaName();
    return;
  }
  size_t len = 0;
  if (nvs_get_str(h, "lastPath", nullptr, &len) == ESP_OK && len > 0) {
    std::string tmp;
    tmp.resize(len);
    size_t out_len = len;
    if (nvs_get_str(h, "lastPath", tmp.data(), &out_len) == ESP_OK) {
      // nvs includes trailing NUL in length; strip it.
      if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
      lastMangaPath = tmp;
    }
  }
  int32_t pg = 0;
  if (nvs_get_i32(h, "lastPage", &pg) == ESP_OK) lastPage = (int)pg;
  nvs_close(h);
  updateLastMangaName();
}

void updateLastMangaName() {
  if (lastMangaPath.empty()) {
    lastMangaName = "";
    return;
  }
  size_t slash = lastMangaPath.rfind('/');
  if (slash != std::string::npos)
    lastMangaName = lastMangaPath.substr(slash + 1);
  else
    lastMangaName = lastMangaPath;
}

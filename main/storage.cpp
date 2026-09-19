#include "storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "compat.h"
#include "cbz.h"
#include "esp_heap_caps.h"
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
  // 4000 capped each SPI DMA transaction at ~7 sectors, which throttled
  // the multi-block reads readFileToBuffer() now issues.
  bus_cfg.max_transfer_sz = 32 * 1024;

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
    // Folders and .cbz archives are both library entries.
    struct stat st = {};
    std::string full = std::string(MANGA_ROOT) + "/" + name;
    if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      mangaFolders.push_back(name);
      ESP_LOGI(TAG, "  Folder: %s", name.c_str());
    } else if (S_ISREG(st.st_mode) && cbz_is_cbz_path(name)) {
      mangaFolders.push_back(name);
      ESP_LOGI(TAG, "  CBZ: %s", name.c_str());
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

// FAT long file names reach 255 characters, so "/sdcard/manga/<name>" can
// exceed the 256-byte stack buffers this used to build paths in. snprintf
// truncated silently, so page probes hit the wrong (nonexistent) path and a
// long-named book read as empty. Build the file name separately and append.
static std::string pageFileName(int n) {
  char nm[32];
  snprintf(nm, sizeof(nm), "/%s%0*d%s", IMG_PREFIX, IMG_DIGITS, n, IMG_SUFFIX);
  return std::string(nm);
}

std::string makePagePath(const std::string &folder, int n) {
  return folder + pageFileName(n);
}

static bool pageExistsFast(const std::string &folder, int n) {
  return access((folder + pageFileName(n)).c_str(), F_OK) == 0;
}

bool pageExists(const std::string &folder, int n) {
  return access(makePagePath(folder, n).c_str(), F_OK) == 0;
}

bool isCbzPath(const std::string &mangaPath) {
  return cbz_is_cbz_path(mangaPath);
}

std::string displayName(const std::string &entry) {
  if (entry.size() > 4 && cbz_is_cbz_path(entry))
    return entry.substr(0, entry.size() - 4);
  return entry;
}

bool bookCoverSource(const std::string &entry, uint32_t &fsize,
                     uint32_t &fmtime) {
  std::string base = std::string(MANGA_ROOT) + "/" + entry;
  std::string p = isCbzPath(base) ? base : makePagePath(base, 0);
  struct stat st = {};
  if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  fsize = (uint32_t)st.st_size;
  fmtime = (uint32_t)st.st_mtime;
  return true;
}

// Shared PSRAM buffer for folder pages (reused across pages/thumbnails).
static uint8_t *jpgBuffer = nullptr;
static size_t jpgBufferSize = 0;

static void ensureJpgBuffer(size_t size) {
  if (jpgBufferSize < size) {
    if (jpgBuffer) heap_caps_free(jpgBuffer);
    // 64-byte aligned: lets sdmmc DMA straight into PSRAM when it can, and
    // keeps chunk pointers cache-line aligned for the bounce path below.
    size = (size + 63) & ~(size_t)63;
    jpgBuffer = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    jpgBufferSize = jpgBuffer ? size : 0;
  }
}

// Bulk file read, replacing fread().
//
// Why not fread: FATFS reports st_blksize = CONFIG_FATFS_VFS_FSTAT_BLKSIZE,
// which defaults to 0 -> newlib falls back to BLKSIZ = 128 bytes. So a
// 600 KB page was pulled in ~4800 read() syscalls, and f_read() of 128
// bytes never reaches its multi-sector direct path -- every 512-byte sector
// went through fs->win as a separate single-block SD transaction (~1200 of
// them). CHUNK-sized reads let FATFS hand whole runs of sectors to
// disk_read() in one CMD18.
//
// The chunk buffer is internal + DMA-capable on purpose:
// sdmmc_read_sectors() falls back to a one-sector-at-a-time loop for any
// destination that fails esp_dma_is_buffer_alignment_satisfied(), and a
// plain MALLOC_CAP_SPIRAM pointer usually does. Bouncing through internal
// RAM costs one memcpy (~15 ms for 600 KB) and guarantees the fast path.
#define SD_READ_CHUNK (32 * 1024)

size_t readFileToBuffer(const char *path, uint8_t *dst, size_t size) {
  if (!dst || size == 0) return 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;

  uint8_t *chunk = (uint8_t *)heap_caps_aligned_alloc(
      64, SD_READ_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  size_t done = 0;

  if (chunk) {
    while (done < size) {
      size_t want = size - done;
      if (want > SD_READ_CHUNK) want = SD_READ_CHUNK;
      ssize_t n = read(fd, chunk, want);
      if (n <= 0) break;
      memcpy(dst + done, chunk, (size_t)n);
      done += (size_t)n;
    }
    heap_caps_free(chunk);
  } else {
    // No internal RAM for the bounce: read straight into the destination.
    // Still far better than 128-byte stdio chunks.
    while (done < size) {
      size_t want = size - done;
      if (want > SD_READ_CHUNK) want = SD_READ_CHUNK;
      ssize_t n = read(fd, dst + done, want);
      if (n <= 0) break;
      done += (size_t)n;
    }
  }

  close(fd);
  return done;
}

size_t loadFileToJpgBuffer(const char *path) {
  struct stat st = {};
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) return 0;

  size_t sz = (size_t)st.st_size;
  ensureJpgBuffer(sz + 1024);
  if (!jpgBuffer) return 0;

  return (readFileToBuffer(path, jpgBuffer, sz) == sz) ? sz : 0;
}

uint8_t *jpgSharedBuffer() { return jpgBuffer; }

// One-slot open-archive cache: reader and preloader hit the same book
// back-to-back, so keep its parsed central directory around.
static CbzArchive *s_cbz = nullptr;
static std::string s_cbz_path;

static CbzArchive *cbz_cached_open(const std::string &path) {
  if (s_cbz && s_cbz_path == path) return s_cbz;
  cbz_close(s_cbz);
  s_cbz = nullptr;
  s_cbz = cbz_open(path);
  if (s_cbz) {
    s_cbz_path = path;
  } else {
    s_cbz_path = "";
  }
  return s_cbz;
}

PageData loadPageData(const std::string &mangaPath, int page) {
  PageData p;
  if (page < 0) return p;
  if (isCbzPath(mangaPath)) {
    CbzArchive *a = cbz_cached_open(mangaPath);
    if (a) p.size = cbz_extract(a, (size_t)page, &p.buf);
    p.owned = (p.buf != nullptr);
    return p;
  }
  p.size = loadFileToJpgBuffer(makePagePath(mangaPath, page).c_str());
  p.buf = (p.size > 0) ? jpgBuffer : nullptr;
  p.owned = false;
  return p;
}

void freePageData(PageData &p) {
  if (p.owned && p.buf) heap_caps_free(p.buf);
  p.buf = nullptr;
  p.size = 0;
  p.owned = false;
}

static std::string s_chap_path;
static ChapterList s_chapters;

const ChapterList &getChapters(const std::string &mangaPath) {
  if (mangaPath != s_chap_path) {
    s_chapters.names.clear();
    s_chapters.starts.clear();
    s_chap_path = mangaPath;
    if (isCbzPath(mangaPath)) {
      if (CbzArchive *a = cbz_cached_open(mangaPath)) {
        s_chapters = cbz_chapters(a);
      }
    }
  }
  return s_chapters;
}

int chapterIndexForPage(const std::string &mangaPath, int page) {
  const ChapterList &ch = getChapters(mangaPath);
  if (ch.names.empty()) return -1;
  if (page < 0) page = 0;
  auto it = std::upper_bound(ch.starts.begin(), ch.starts.end(), page);
  int idx = (int)(it - ch.starts.begin()) - 1;
  return (idx < 0) ? 0 : idx;
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

  if (isCbzPath(folder)) {
    count = 0;
    CbzArchive *a = cbz_cached_open(folder);
    if (a) count = (int)a->images.size();
    cachedFolder = folder;
    cachedCount = count;
    storeCachedPageCount(folder, count);
    return count;
  }

  if (!pageExistsFast(folder, 0)) {
    cachedFolder = folder;
    cachedCount = 0;
    storeCachedPageCount(folder, 0);
    return 0;
  }

  int hi = 1;
  while (pageExistsFast(folder, hi)) {
    hi *= 2;
    if (hi > 100000) {
      hi = 100000;
      break;
    }
  }

  int lo = hi / 2;
  while (lo + 1 < hi) {
    int mid = lo + (hi - lo) / 2;
    if (pageExistsFast(folder, mid))
      lo = mid;
    else
      hi = mid;
  }

  cachedFolder = folder;
  cachedCount = lo + 1;
  storeCachedPageCount(folder, cachedCount);
  return cachedCount;
}

// Progress is throttled to spare NVS flash wear while paging quickly, but a
// throttled call used to be dropped for good: the RAM copy advanced while the
// persisted copy stayed behind, so the last few page turns before a
// power-off or reset were lost. A skipped write is now remembered as pending
// and flushed by the next call that lands outside the window, or explicitly
// via saveProgress(true) / flushProgress().
static bool s_progressDirty = false;

static void writeProgressToNvs() {
  nvs_handle_t h;
  if (nvs_open("manga", NVS_READWRITE, &h) != ESP_OK) return;
  bool ok = nvs_set_str(h, "lastPath", lastMangaPath.c_str()) == ESP_OK &&
            nvs_set_i32(h, "lastPage", (int32_t)lastPage) == ESP_OK &&
            nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (ok) s_progressDirty = false;
}

void saveProgress(bool force) {
  static uint32_t lastSaveMs = 0;
  static bool everSaved = false;
  uint32_t now = idf_millis();
  lastMangaPath = currentMangaPath;
  lastPage = currentPage;
  updateLastMangaName();
  s_progressDirty = true;
  // Unsigned subtraction is wrap-safe. everSaved keeps the very first write
  // from being throttled against a lastSaveMs of 0 (millis() < 2 s after boot).
  if (!force && everSaved && now - lastSaveMs < 2000) return;
  lastSaveMs = now;
  everSaved = true;
  writeProgressToNvs();
}

void flushProgress() {
  if (s_progressDirty) writeProgressToNvs();
}

void loadProgress() {
  nvs_handle_t h;
  if (nvs_open("manga", NVS_READONLY, &h) != ESP_OK) {
    updateLastMangaName();
    return;
  }
  size_t len = 0;
  // Cap: a corrupt NVS length must never drive a huge std::string alloc
  // (allocation failure aborts — no exceptions in firmware). Paths here
  // are always short "/sdcard/manga/<name>" strings.
  if (nvs_get_str(h, "lastPath", nullptr, &len) == ESP_OK && len > 0 &&
      len <= 512) {
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

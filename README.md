# M5MangaS3 +
A manga reader for M5PaperS3 - a fork of the old, unmaintained M5Manga.

Pure ESP-IDF app (`main/`, entry `app_main` in `main/main.cpp`).
Originally ported from an Arduino sketch; the old `src/` + `platformio.ini`
were removed once the port was complete (still in git history if needed).

## Build (M5PaperS3, ESP32-S3, 16MB flash, 8MB OPI PSRAM)

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` already selects ESP32-S3, 16MB QIO flash, OPI PSRAM,
USB-CDC console and the custom `partitions.csv` (15MB factory app).
Dependencies (`m5stack/m5unified`, `m5stack/m5gfx`) come from the ESP
component registry and are pinned in `dependencies.lock`.

## Library layout (`/sdcard/manga/`)

Each entry is either a folder of `m5_0000.jpg`, `m5_0001.jpg`, … files or a
`.cbz` archive (ZIP with JPG/PNG images, any filenames, read in natural
sort order). CBZ pages are extracted on demand straight to PSRAM — nothing
is unpacked onto the SD card — using a minimal ZIP reader (`main/cbz.cpp`)
with DEFLATE handled by the ESP32-S3 ROM tinfl. Encrypted, multi-disk and
ZIP64 archives are refused. `.cbz` files can be uploaded via the WiFi file
browser like any other file. CBZ pages grouped in top-level folders become
navigable chapters: the Book Menu (swipe up from the bottom while reading)
gains a `< Chapter (i/n) >` stepper below the page changer that jumps to the
neighboring chapter's first page. Flat archives and folder books show
`NO CHAPTERS` instead.

Covers are cached per book in `/sdcard/.thumbs/` as pre-fitted 221x313
8-bit raw dumps: the first menu visit decodes page 0 once, later visits do
a single fread + memcpy with no image decode. Cache keys embed the cover's
size+mtime for automatic invalidation (plus a format magic), orphans are
purged on book delete and capped at 256 files.

## Porting notes (Arduino -> IDF)

- `String` -> `std::string`; `File`/`SD.h` -> POSIX `fopen`/`opendir`/`stat`
  on the SDMMC mount `/sdcard` (`MANGA_ROOT=/sdcard/manga`,
  `PIC_ROOT=/sdcard/pic`).
- `Preferences` -> NVS (`nvs_flash`, namespace `manga`: `lastPath`, `lastPage`).
- `WiFi.h` + `WebServer` -> `esp_wifi` soft-AP + `esp_http_server`
  (streaming multipart upload parser, recursive delete, `mkdir -p`).
- JPEG decode is bitbank2/JPEGDEC, vendored under `components/jpegdec`
  (upstream `86282979`; ESP-IDF 6 `-Werror` fixes + S3-SIMD wiring documented
  in its CMakeLists). Full-res pages render through it; PNG and thumbnails
  stay on M5GFX's built-in codecs.
- `millis`/`delay`/`random` -> `esp_timer_get_time`/`vTaskDelay`/`esp_random`
  (see `main/compat.h`); `setCpuFrequencyMhz()` is a no-op, clock scaling is
  left to `CONFIG_PM_ENABLE`.
- `setup()`/`loop()` -> `app_main()` with a FreeRTOS loop; display, touch,
  power and EPD APIs are unchanged (M5Unified/M5GFX work on both frameworks).
- Quality refresh is Kindle-style: `tools/patch_epd_lut.py` (run at configure
  time, hash-pinned to the M5GFX version in `dependencies.lock`) replaces the
  stock ~36-scan `lut_eraser`/`lut_quality` tables with a short black flash
  -> white -> image script (~18 scans). See the script header for tuning and
  revert instructions.
- `fullRefresh()` (ui.cpp) renders through a *solid* full waveform
  for the power-off splash that persists on screen: the same script keeps
  long-saturate black/white tables plus a `Panel_EPD::refreshStockWaveform()`
  one-shot (frame counts tunable via `SOLID_*_FRAMES`). No switch-back is
  needed; reboot re-expands the Kindle tables.

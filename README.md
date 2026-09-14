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

## Porting notes (Arduino -> IDF)

- `String` -> `std::string`; `File`/`SD.h` -> POSIX `fopen`/`opendir`/`stat`
  on the SDMMC mount `/sdcard` (`MANGA_ROOT=/sdcard/manga`,
  `PIC_ROOT=/sdcard/pic`).
- `Preferences` -> NVS (`nvs_flash`, namespace `manga`: `lastPath`, `lastPage`).
- `WiFi.h` + `WebServer` -> `esp_wifi` soft-AP + `esp_http_server`
  (streaming multipart upload parser, recursive delete, `mkdir -p`).
- `JPEGDEC` (bitbank2, breaks on IDF 6 `-Werror`) dropped in favour of
  M5GFX's built-in TJpgD (`sprite.drawJpg()`), then the same
  contrast/dither pipeline runs on the sprite.
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

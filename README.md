# M5MangaS3 +
A manga reader for M5PaperS3 - a fork of the old, unmaintained M5Manga.

## Builds

- **Arduino / PlatformIO** (original): `src/` + `platformio.ini`.
- **Pure ESP-IDF** (no Arduino): `main/` + `CMakeLists.txt`.
  Requires ESP-IDF v5.1+ with M5Unified/M5GFX from the component registry:

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

ESP-IDF port notes: `String` -> `std::string`, `SD.h` -> `esp_vfs_fat_sdspi`
(mounted at `/sdcard`, 80/40/20MHz fallback), `Preferences` -> NVS,
`WebServer`/`WiFi` -> `esp_http_server`/`esp_wifi`, `JPEGDEC` -> vendored
under `components/jpegdec` (same upstream sources, relaxed `-Werror` for
IDF v6), `millis`/`delay` -> `esp_timer`/`vTaskDelay`, `setCpuFrequencyMhz` ->
`esp_pm` lock hints (`main/compat.h`).

Display notes: M5GFX >= 0.2.28 rewrote the PaperS3 EPD engine and stretched
the quality waveform (20 -> 31 phases), making page turns ~55% slower than
the Arduino build's M5GFX 0.2.7. `tools/patch_m5gfx_epd.py` (run at configure
time, hash-pinned) restores the 0.2.7 `Panel_EPD` engine + waveforms on top
of the current IDF6-compatible bus layer for Arduino-grade refresh speed.

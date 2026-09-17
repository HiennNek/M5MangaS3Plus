# Kindle-style EPD quality waveform (see tools/patch_epd_lut.py).
# Replaces the stock ~36-scan lut_eraser/lut_quality tables in the managed
# M5GFX copy with a short black -> white -> image script (~18 scans).
# Fails configure loudly if the M5GFX version changed (hash-pinned).
execute_process(
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_epd_lut.py
    RESULT_VARIABLE _epd_lut_patch_res
    OUTPUT_VARIABLE _epd_lut_patch_out
    ERROR_VARIABLE _epd_lut_patch_out)
message(STATUS "M5GFX EPD LUT patch: ${_epd_lut_patch_out}")
if(NOT _epd_lut_patch_res EQUAL 0)
    message(FATAL_ERROR "M5GFX EPD LUT patch failed: ${_epd_lut_patch_out}")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "state.cpp"
        "storage.cpp"
        "bookmarks.cpp"
        "ui.cpp"
        "input.cpp"
        "wifi_server.cpp"
        "cbz.cpp"
        "thumb.cpp"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        m5unified
        m5gfx
        jpegdec
        esp_rom
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        fatfs
        sdmmc
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        esp_psram
        heap
)
# M5PaperS3 board select for M5Unified (mirrors -DBOARD_M5PAPERS3 from Arduino)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5PAPERS3=1)

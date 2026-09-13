# Restore the M5GFX 0.2.7 EPD engine (see tools/patch_m5gfx_epd.py).
# Fails configure loudly if the vendored M5GFX version changed.
execute_process(
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_m5gfx_epd.py
    RESULT_VARIABLE _lut_patch_res
    OUTPUT_VARIABLE _lut_patch_out
    ERROR_VARIABLE _lut_patch_out)
message(STATUS "M5GFX EPD patch: ${_lut_patch_out}")
if(NOT _lut_patch_res EQUAL 0)
    message(FATAL_ERROR "M5GFX EPD patch failed: ${_lut_patch_out}")
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
        "compat.cpp"
        "jpeg_helper.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        m5unified
        jpegdec
        fatfs
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        driver
        esp_pm
)

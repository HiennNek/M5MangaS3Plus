# ESP-IDF project file for M5MangaS3Plus (pure ESP-IDF, no Arduino).
#
# Build with:
#   idf.py set-target esp32s3
#   idf.py build
#   idf.py flash monitor
#
# Dependencies (M5Unified/M5GFX) are pulled from the ESP Component Registry
# via main/idf_component.yml. Requires ESP-IDF v5.1+.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5manga)

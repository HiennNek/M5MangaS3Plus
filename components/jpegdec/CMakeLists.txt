# Vendored bitbank2/JPEGDEC + ESP-IDF glue.
#
# Upstream: https://github.com/bitbank2/JPEGDEC (Apache-2.0, see LICENSE).
# The registry release (1.6.2) predates ESP-IDF 6 and neither builds under
# it (-Werror on pointer-to-int-cast / shift-negative-value) nor wires up
# the ESP32-S3 SIMD path without Arduino. This component fixes both with a
# minimal footprint:
#   - src/        : verbatim upstream sources needed for RAM decoding
#                   (JPEGDEC.cpp/.h, jpeg.inl, s3_simd_420/444.S).
#                   NOT included: s3_simd_dequant.S (no callers),
#                   JPEGDisplay.* (unused helper), examples.
#   - shim/       : dsps_fft2r_platform.h stand-in answering the single
#                   S3-AES capability macro JPEGDEC gates its SIMD on
#                   (avoids depending on all of espressif/esp-dsp).
#   - Two -Werror fixes in jpeg.inl (documented at the sites).
# No Arduino dependency: on ESP_PLATFORM the header uses libc, and the
# File-based open() overload compiles out (no FS.h on IDF paths).
#
# ESP32-S3 only: the AES assembly has no meaning elsewhere. (IDF_TARGET,
# not CONFIG_IDF_TARGET_*, which does not exist yet at configure time.)
if(NOT "${IDF_TARGET}" STREQUAL "esp32s3")
  message(FATAL_ERROR "components/jpegdec requires ESP32-S3 (S3 AES SIMD)")
endif()

idf_component_register(
  SRCS
    "src/JPEGDEC.cpp"
    "src/s3_simd_420.S"
    "src/s3_simd_444.S"
  INCLUDE_DIRS
    "src"
    "shim"
)

# Enables JPEGDEC's ESP32S3_SIMD code paths (gated on this macro upstream).
# Private: dependents only need JPEGDEC.h, which is macro-free.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ARDUINO_ARCH_ESP32)

#pragma once
// Stand-in for esp-dsp's dsps_fft2r_platform.h.
//
// bitbank2/JPEGDEC only tests the macro below to decide whether the
// ESP32-S3 AES-accelerated YCbCr routines (s3_simd_*.S, raw assembly using
// no library calls) may be used. Depending on all of espressif/esp-dsp
// just for this gate would add minutes to every build, so this header
// answers it directly. Unconditional by design: this component is fenced
// to ESP32-S3 in its CMakeLists, and every S3 has the AES peripheral
// these routines use.
#define dsps_fft2r_sc16_aes3_enabled 1

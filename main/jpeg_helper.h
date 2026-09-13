#pragma once
// JPEG file helpers for pure ESP-IDF.
//
// Full pages use the fast bitbank2 JPEGDEC path in ui.cpp (same as the
// Arduino build). These helpers cover the remaining cases (library
// thumbnails, shutdown splash) via M5GFX's built-in drawJpg (TJpgDec, no
// Arduino dependency), and only bridge VFS files (-> RAM buffer in PSRAM)
// to that API.

#include <M5GFX.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Load a whole file from VFS into a PSRAM buffer.
// Caller must release(*out_buf) with heap_caps_free(). False on failure.
bool load_file_to_ram(const std::string &path, uint8_t **out_buf,
                      size_t *out_size);

// Draw a JPEG file (VFS path) onto a sprite. w/h follow M5GFX drawJpg
// semantics (0 = native size, clipped to the sprite). The sprite is NOT
// cleared first - fill it yourself if the image may be smaller than the
// sprite. Returns false if the file can't be read or decoded.
bool draw_jpg_file(LGFX_Sprite &spr, const std::string &path, int32_t x,
                   int32_t y, int32_t w = 0, int32_t h = 0);

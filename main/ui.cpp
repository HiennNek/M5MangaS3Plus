#include "ui.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bookmarks.h"
#include "compat.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "icon.h"
#include "state.h"
#include "storage.h"
#include "thumb.h"
#include "wifi_server.h"
#include "M5Unified.h"
// Panel_EPD for refreshStockWaveform() (injected by tools/patch_epd_lut.py).
#include "lgfx/v1/platforms/esp32/Panel_EPD.hpp"

static const char *TAG = "ui";

// Vendored bitbank2/JPEGDEC (components/jpegdec): SIMD-accelerated JPEG
// decoding on ESP32-S3. PNG stays on M5GFX's built-in decoder.
#include <JPEGDEC.h>

// Software fallback for oversized pages (components/stbimage): stb_image
// decodes the full frame to grayscale in PSRAM so ui.cpp can scale it to
// fit. JPEGDEC's power-of-two DCT scaling and M5GFX's affine scaler cannot
// reliably handle arbitrary larger-than-screen sizes (e.g. 667x1200 on a
// 540x960 panel) and fault with a LoadStoreError.
#include "stb_image.h"

// One decoder per task: JPEGDEC keeps decode state in the object, and the
// main task (page render) and the preload worker can decode concurrently.
static JPEGDEC s_jpegMain;
static JPEGDEC s_jpegWorker;

// Full-screen zoom scratch (PAGE_DEPTH, ~0.5MB). File scope so book-switch
// cleanup can free it; drawZoomed() re-creates it on demand.
static LGFX_Sprite s_zoomSprite(&M5.Display);

// Image decoding: JPEG via JPEGDEC straight into the target sprite (format
// is sniffed from magic bytes so CBZ entries work regardless of their file
// extension), PNG via M5GFX. Then contrast/gray runs on the sprite buffer.
static bool isJpeg(const uint8_t *buf, size_t size) {
  return size >= 2 && buf[0] == 0xFF && buf[1] == 0xD8;
}
static bool isPng(const uint8_t *buf, size_t size) {
  return size >= 4 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' &&
         buf[3] == 'G';
}

// Page post-processing (contrast preset + gray-level quantization) used to
// be two extra full-frame passes over the 540x960x16bpp sprite *in PSRAM*:
// ~1 MB read + 1 MB written, twice, plus a vTaskDelay(1) every 64K pixels.
// Both are pure per-pixel gray->gray maps, so they compose into one 256-entry
// LUT that is applied inside drawMCU() while the block is still in internal
// RAM. Same output, zero extra PSRAM traffic.
// JPEGDEC now emits 8-bit luma (see decodeJpegToSprite), so the table maps
// luma -> finished big-endian RGB565 sprite pixel in one indexed load: no
// RGB unpack, no luma math, no second pass.
struct PixelLut {
  uint8_t map[256];
};

static inline int lumaToRgb565(int v) {
  return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
}
static inline int rgb565ToLuma(int px) {
  int r = (px >> 11) << 3, g = ((px >> 5) & 0x3F) << 2, b = (px & 0x1F) << 3;
  return (r * 306 + g * 601 + b * 117) >> 10;
}
// What Panel_EPD stores for an rgb565 sprite pixel: swap565_t's R8/G8/B8 (bit
// replication, not a plain shift) fed through grayscale_t(r,g,b), which is
// (r + 2g + b) >> 2 -- a different weighting from the BT.601 luma above.
// Baking this in is what keeps a grayscale sprite showing the same pixels the
// rgb565 one did.
static inline uint8_t rgb565ToPanelGray(int px) {
  const int r5 = px >> 11, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
  const int gh = g6 >> 3, gl = g6 & 7;
  const int r8 = (r5 << 3) + (r5 >> 2);
  const int g8 = (((gh << 3) + gl) << 2) + (gh >> 1);
  const int b8 = (b5 << 3) + (b5 >> 2);
  return (uint8_t)((r8 + (g8 << 1) + b8) >> 2);
}

// Built stage for stage against the old pipeline -- JPEGDEC's luma->RGB565
// table, then applyContrast, then applyGrayLevels -- including the lossy
// 5/6/5 round-trip each stage used to go through. Skipping those round-trips
// looks harmless but shifts values across quantization boundaries: at 4 gray
// levels it moved 8 of 256 luma values a whole level.
static void buildPixelLut(PixelLut &lut, ContrastPreset preset, int levels) {
  const bool doContrast = (preset != CONTRAST_NORMAL);
  const bool doLevels = (levels == 8 || levels == 4);

  int32_t contrast_fp = 256, brightness = 0;
  if (doContrast) {
    switch (preset) {
      case CONTRAST_VIVID:  contrast_fp = 307; brightness = 0;  break;
      case CONTRAST_HIGH:   contrast_fp = 358; brightness = 5;  break;
      case CONTRAST_LIGHT:  contrast_fp = 256; brightness = 30; break;
      default: break;
    }
  }
  const int32_t steps = doLevels ? (levels - 1) : 0;

  for (int i = 0; i < 256; i++) {
    int px = lumaToRgb565(i);
    if (doContrast) {
      int32_t v = (((rgb565ToLuma(px) - 128) * contrast_fp) >> 8) + 128 +
                  brightness;
      if (v < 0) v = 0;
      else if (v > 255) v = 255;
      px = lumaToRgb565((int)v);
    }
    if (doLevels) {
      px = lumaToRgb565((int)(((int32_t)rgb565ToLuma(px) * steps + 127) / 255 *
                              255 / steps));
    }
    lut.map[i] = rgb565ToPanelGray(px);
  }
}

// Single-pass equivalent of applyContrast + applyGrayLevels for an 8-bit
// page sprite (the PNG path; JPEG applies the LUT inside drawMCU).
static void applyPixelLut(LGFX_Sprite &spr, const PixelLut &lut) {
  uint8_t *buf = (uint8_t *)spr.getBuffer();
  if (!buf) return;
  const size_t n = (size_t)spr.width() * spr.height();
  for (size_t i = 0; i < n; i++) buf[i] = lut.map[buf[i]];
}

// ---- Oversized-image handling -------------------------------------------
// Fast paths below (JPEGDEC / M5GFX) clip rather than scale: any source
// larger than the target sprite in either dimension would be top-left
// cropped, and worse, their scalers fault on such inputs (LoadStoreError
// for a 667x1200 JPEG on the 540x960 panel). Images that do not fit are
// therefore routed through stb_image, which decodes the full frame to
// grayscale in PSRAM and scales it to fit with aspect preservation.
//
// Header parsing is decoder-free on purpose: it only reads the SOF/IHDR
// markers, so merely probing a large image can never crash.

// JPEG SOF0-SOF3 (baseline/extended/progressive) carry the size. Other SOF
// markers (except DHT/JPG/DAC) are accepted the same way.
static bool jpegDimensions(const uint8_t *buf, size_t size, int *w, int *h) {
  if (size < 4 || buf[0] != 0xFF || buf[1] != 0xD8) return false;
  size_t pos = 2;
  while (pos + 4 <= size) {
    if (buf[pos] != 0xFF) return false;
    // Skip fill bytes (0xFF padding before the marker).
    while (pos + 1 < size && buf[pos + 1] == 0xFF) pos++;
    if (pos + 4 > size) return false;
    uint8_t marker = buf[pos + 1];
    // Standalone markers carry no length.
    if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    uint16_t len =
        (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
    if (len < 2 || pos + 2 + len > size) return false;
    const bool isSof =
        (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
         marker != 0xC8 && marker != 0xCC);
    if (isSof) {
      if (len < 7) return false;
      *h = (buf[pos + 5] << 8) | buf[pos + 6];
      *w = (buf[pos + 7] << 8) | buf[pos + 8];
      return *w > 0 && *h > 0;
    }
    if (marker == 0xDA) return false;  // SOS before SOF: not a plain image
    pos += 2 + len;
  }
  return false;
}

static bool pngDimensions(const uint8_t *buf, size_t size, int *w, int *h) {
  if (size < 24 || !isPng(buf, size)) return false;
  // 8-byte signature + 4-byte length + "IHDR" + W/H big-endian.
  if (buf[12] != 'I' || buf[13] != 'H' || buf[14] != 'D' ||
      buf[15] != 'R')
    return false;
  *w = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
  *h = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
  return *w > 0 && *h > 0;
}

static bool imageDimensions(const uint8_t *buf, size_t size, int *w,
                            int *h) {
  if (isJpeg(buf, size)) return jpegDimensions(buf, size, w, h);
  if (isPng(buf, size)) return pngDimensions(buf, size, w, h);
  return false;
}

// Progressive JPEG (SOF2) never takes the fast path: JPEGDEC only decodes
// its DC scan (a 1/8-size draft), so even a small progressive page would
// show as a tiny top-left stamp. stb_image decodes them fully.
static bool isProgressiveJpeg(const uint8_t *buf, size_t size) {
  if (!isJpeg(buf, size) || size < 4) return false;
  size_t pos = 2;
  while (pos + 4 <= size) {
    if (buf[pos] != 0xFF) return false;
    while (pos + 1 < size && buf[pos + 1] == 0xFF) pos++;
    if (pos + 4 > size) return false;
    uint8_t marker = buf[pos + 1];
    if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    uint16_t len = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
    if (len < 2 || pos + 2 + len > size) return false;
    if (marker == 0xC2) return true;   // SOF2: progressive
    if ((marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
         marker != 0xC8 && marker != 0xCC))
      return false;  // baseline/extended SOF: not progressive
    if (marker == 0xDA) return false;  // SOS before SOF
    pos += 2 + len;
  }
  return false;
}

// True when the source cannot be shown 1:1 in the target box and must go
// through the fit-to-screen path instead of the clipping fast paths.
// Unknown dimensions for a known JPEG/PNG also take the safe path: stb_image
// fails gracefully while the fast scalers can fault.
static bool needsFitToScreen(const uint8_t *buf, size_t size, int maxWidth,
                             int maxHeight) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  if (isProgressiveJpeg(buf, size)) return true;
  int w = 0, h = 0;
  if (!imageDimensions(buf, size, &w, &h))
    return isJpeg(buf, size) || isPng(buf, size);
  return w > maxWidth || h > maxHeight;
}

// Confirmed oversized for the display (dimensions known and exceeding it).
// Unlike needsFitToScreen() this is false for small progressive/unknown
// images, so it drives the memory policy below: only true oversize pays
// for a w*h PSRAM frame worth disabling the preloader over.
static bool isOversizedPage(const uint8_t *buf, size_t size) {
  int w = 0, h = 0;
  return imageDimensions(buf, size, &w, &h) &&
         (w > DISPLAY_W || h > DISPLAY_H);
}

static inline uint8_t lumaToRgb332(uint8_t v) {
  return (uint8_t)((((v >> 5) << 3) + (v >> 5)) << 2) + (v >> 6);
}

// Bilinear fit of an 8-bit grayscale source into an 8-bit sprite,
// centered with white letterboxing. outW/outH preserve the source aspect
// ratio; dstW/dstH is the sprite pitch. map converts source luma to the
// destination encoding (panel gray for pages, rgb332 for thumbs); pass
// nullptr for identity.
static void blitFitGray(uint8_t *dst, int dstW, int dstH, const uint8_t *src,
                        int srcW, int srcH, int outW, int outH,
                        const uint8_t *map) {
  const int ox = (dstW - outW) / 2;
  const int oy = (dstH - outH) / 2;
  const float xScale = (float)srcW / (float)outW;
  const float yScale = (float)srcH / (float)outH;
  for (int dy = 0; dy < outH; dy++) {
    const float sy = (dy + 0.5f) * yScale - 0.5f;
    int y0 = (int)floorf(sy);
    float fy = sy - (float)y0;
    if (y0 < 0) {
      y0 = 0;
      fy = 0.0f;
    } else if (y0 >= srcH - 1) {
      y0 = srcH - 1;
      fy = 0.0f;
    }
    int y1 = y0 + 1;
    if (y1 >= srcH) y1 = srcH - 1;
    const uint8_t *row0 = src + (size_t)y0 * srcW;
    const uint8_t *row1 = src + (size_t)y1 * srcW;
    uint8_t *drow = dst + (size_t)(oy + dy) * dstW + ox;
    for (int dx = 0; dx < outW; dx++) {
      const float sx = (dx + 0.5f) * xScale - 0.5f;
      int x0 = (int)floorf(sx);
      float fx = sx - (float)x0;
      if (x0 < 0) {
        x0 = 0;
        fx = 0.0f;
      } else if (x0 >= srcW - 1) {
        x0 = srcW - 1;
        fx = 0.0f;
      }
      int x1 = x0 + 1;
      if (x1 >= srcW) x1 = srcW - 1;
      const int p00 = row0[x0], p10 = row0[x1];
      const int p01 = row1[x0], p11 = row1[x1];
      const float top = (float)p00 + (float)(p10 - p00) * fx;
      const float bot = (float)p01 + (float)(p11 - p01) * fx;
      int v = (int)(top + (bot - top) * fy + 0.5f);
      if (v < 0)
        v = 0;
      else if (v > 255)
        v = 255;
      drow[dx] = map ? map[v] : (uint8_t)v;
    }
  }
}

// Large-image page path: decode via stb_image to 1-channel grayscale in
// PSRAM, then bilinear-fit into the page sprite with the contrast/gray LUT
// folded in. Covers JPEG (baseline + progressive) and PNG; the fast paths
// cannot scale these reliably.
static bool decodeLargeFitToPage(LGFX_Sprite &spr, uint8_t *buf, size_t size,
                                 int x, int y, int maxWidth, int maxHeight,
                                 const PixelLut *lut) {
  if (!buf || size == 0 || size > (size_t)INT32_MAX) return false;
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  uint8_t *dst = (uint8_t *)spr.getBuffer();
  if (!dst) return false;

  // Early space check from the header probe: the stb frame needs w*h
  // contiguous PSRAM bytes, and on an 8MB part a huge panorama can never
  // fit no matter what we free. Fail fast with an error message instead
  // of churning through a doomed decode.
  {
    int probeW = 0, probeH = 0;
    if (imageDimensions(buf, size, &probeW, &probeH)) {
      if (probeW > 5000 || probeH > 5000 ||
          (size_t)probeW * (size_t)probeH > 10u * 1024u * 1024u) {
        ESP_LOGW(TAG, "large image %dx%d exceeds fit cap", probeW, probeH);
        return false;
      }
      const size_t need = (size_t)probeW * (size_t)probeH;
      const size_t largest =
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
      if (need > largest) {
        ESP_LOGW(TAG, "large image %dx%d needs %u contig, largest free %u",
                 probeW, probeH, (unsigned)need, (unsigned)largest);
        return false;
      }
    }
  }

  int srcW = 0, srcH = 0;
  // Force 1 channel: halves PSRAM vs RGB and matches the gray panel.
  uint8_t *src = stbi_load_from_memory(buf, (int)size, &srcW, &srcH,
                                       nullptr, 1);
  if (!src || srcW <= 0 || srcH <= 0) {
    if (src) stbi_image_free(src);
    return false;
  }
  // Guard PSRAM: a pathological panorama must fail with an error message,
  // never with an OOM abort deep in the decoder.
  const size_t pxCount = (size_t)srcW * (size_t)srcH;
  if (srcW > 5000 || srcH > 5000 || pxCount > 10u * 1024u * 1024u) {
    ESP_LOGW(TAG, "large image %dx%d exceeds fit cap", srcW, srcH);
    stbi_image_free(src);
    return false;
  }

  const float fitScale =
      std::min((float)maxWidth / (float)srcW, (float)maxHeight / (float)srcH);
  // Never upscale: unknown-dimension small images reach this path via the
  // conservative needsFitToScreen() probe and must stay 1:1, like the fast
  // path would show them.
  const float scale = fitScale > 1.0f ? 1.0f : fitScale;
  int outW = (int)((float)srcW * scale + 0.5f);
  int outH = (int)((float)srcH * scale + 0.5f);
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
  if (outW > maxWidth) outW = maxWidth;
  if (outH > maxHeight) outH = maxHeight;

  spr.fillScreen(TFT_WHITE);
  // Pages always pass (0,0,DISPLAY_W,DISPLAY_H) with a sprite of the same
  // size, so centering in the sprite is centering in the caller's box.
  // The x/y/maxWidth/maxHeight parameters are kept for signature parity
  // with the fast path; assert the assumption instead of silently
  // mis-centering if a future caller changes it.
  const int sprW = spr.width(), sprH = spr.height();
  (void)x;
  (void)y;
  if (sprW != maxWidth || sprH != maxHeight) {
    ESP_LOGW(TAG, "large-fit box %dx%d != sprite %dx%d", maxWidth, maxHeight,
             sprW, sprH);
  }
  blitFitGray(dst, sprW, sprH, src, srcW, srcH, outW, outH, lut->map);
  ESP_LOGI(TAG, "large image %dx%d -> %dx%d fit", srcW, srcH, outW, outH);
  stbi_image_free(src);
  return true;
}

// Large-image thumbnail path: same decode, but the thumb sprite is rgb332
// (see UI_DEPTH), so luma is converted to rgb332 instead of panel gray.
// Thumbnails ignore contrast/gray settings by design.
static bool decodeLargeFitToThumb(LGFX_Sprite &spr, uint8_t *buf,
                                  size_t size) {
  if (!buf || size == 0 || size > (size_t)INT32_MAX) return false;
  uint8_t *dst = (uint8_t *)spr.getBuffer();
  if (!dst) return false;
  const int dstW = spr.width(), dstH = spr.height();

  int srcW = 0, srcH = 0;
  uint8_t *src = stbi_load_from_memory(buf, (int)size, &srcW, &srcH,
                                       nullptr, 1);
  if (!src || srcW <= 0 || srcH <= 0) {
    if (src) stbi_image_free(src);
    return false;
  }
  const size_t pxCount = (size_t)srcW * (size_t)srcH;
  if (srcW > 5000 || srcH > 5000 || pxCount > 10u * 1024u * 1024u) {
    stbi_image_free(src);
    return false;
  }
  const float fitScaleThumb =
      std::min((float)dstW / (float)srcW, (float)dstH / (float)srcH);
  const float scale = fitScaleThumb > 1.0f ? 1.0f : fitScaleThumb;
  int outW = (int)((float)srcW * scale + 0.5f);
  int outH = (int)((float)srcH * scale + 0.5f);
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
  if (outW > dstW) outW = dstW;
  if (outH > dstH) outH = dstH;

  spr.fillScreen(TFT_WHITE);
  // Convert on the fly: bilinear in luma, then luma->rgb332 per pixel.
  const int ox = (dstW - outW) / 2;
  const int oy = (dstH - outH) / 2;
  const float xScale = (float)srcW / (float)outW;
  const float yScale = (float)srcH / (float)outH;
  for (int dy = 0; dy < outH; dy++) {
    const float sy = (dy + 0.5f) * yScale - 0.5f;
    int y0 = (int)floorf(sy);
    float fy = sy - (float)y0;
    if (y0 < 0) {
      y0 = 0;
      fy = 0.0f;
    } else if (y0 >= srcH - 1) {
      y0 = srcH - 1;
      fy = 0.0f;
    }
    int y1 = y0 + 1;
    if (y1 >= srcH) y1 = srcH - 1;
    const uint8_t *row0 = src + (size_t)y0 * srcW;
    const uint8_t *row1 = src + (size_t)y1 * srcW;
    uint8_t *drow = dst + (size_t)(oy + dy) * dstW + ox;
    for (int dx = 0; dx < outW; dx++) {
      const float sx = (dx + 0.5f) * xScale - 0.5f;
      int x0 = (int)floorf(sx);
      float fx = sx - (float)x0;
      if (x0 < 0) {
        x0 = 0;
        fx = 0.0f;
      } else if (x0 >= srcW - 1) {
        x0 = srcW - 1;
        fx = 0.0f;
      }
      int x1 = x0 + 1;
      if (x1 >= srcW) x1 = srcW - 1;
      const int p00 = row0[x0], p10 = row0[x1];
      const int p01 = row1[x0], p11 = row1[x1];
      const float top = (float)p00 + (float)(p10 - p00) * fx;
      const float bot = (float)p01 + (float)(p11 - p01) * fx;
      int v = (int)(top + (bot - top) * fy + 0.5f);
      if (v < 0)
        v = 0;
      else if (v > 255)
        v = 255;
      drow[dx] = lumaToRgb332((uint8_t)v);
    }
  }
  stbi_image_free(src);
  return true;
}

struct JpegDrawContext {
  LGFX_Sprite *spr;
  int offsetX;
  int offsetY;
  int maxWidth;
  int maxHeight;
  const PixelLut *lut;
};

// JPEGDEC emits RGB565 big-endian blocks, matching the sprite buffer
// layout, so blocks are copied row by row with clipping (no conversion).
static int drawMCU(JPEGDRAW *pDraw) {
  auto *ctx = (JpegDrawContext *)pDraw->pUser;
  LGFX_Sprite *spr = ctx->spr;
  const int sprW = spr->width();
  const int sprH = spr->height();

  int cw = pDraw->iWidth;
  int ch = pDraw->iHeight;
  if (ctx->maxWidth > 0 && pDraw->x + cw > ctx->maxWidth)
    cw = ctx->maxWidth - pDraw->x;
  if (ctx->maxHeight > 0 && pDraw->y + ch > ctx->maxHeight)
    ch = ctx->maxHeight - pDraw->y;
  if (cw <= 0 || ch <= 0) return 1;

  int outX = pDraw->x + ctx->offsetX;
  int outY = pDraw->y + ctx->offsetY;
  if (outX < 0) {
    cw += outX;
    outX = 0;
  }
  if (outY < 0) {
    ch += outY;
    outY = 0;
  }
  if (outX + cw > sprW) cw = sprW - outX;
  if (outY + ch > sprH) ch = sprH - outY;
  if (cw <= 0 || ch <= 0) return 1;

  // iWidthUsed is the valid width of this block; iWidth is the buffer pitch
  // and carries MCU padding past the right edge.
  if (pDraw->iWidthUsed > 0 && cw > pDraw->iWidthUsed) cw = pDraw->iWidthUsed;
  if (cw <= 0) return 1;

  // Both sides are now 8-bit luma, so this is a byte-for-byte write into the
  // sprite instead of the 2 bytes per pixel the rgb565 sprite needed.
  const uint8_t *pixels = (const uint8_t *)pDraw->pPixels;  // iBpp == 8
  uint8_t *dst = (uint8_t *)spr->getBuffer();
  const uint8_t *map = ctx->lut->map;

  for (int y = 0; y < ch; y++) {
    const uint8_t *src = &pixels[y * pDraw->iWidth];
    uint8_t *out = &dst[(outY + y) * sprW + outX];
    for (int x = 0; x < cw; x++) out[x] = map[src[x]];
  }
  return 1;
}

static bool decodeJpegToSprite(JPEGDEC &dec, LGFX_Sprite &spr, uint8_t *buf,
                               size_t size, int x, int y, int maxWidth,
                               int maxHeight, const PixelLut *lut) {
  JpegDrawContext ctx = {&spr, x, y, maxWidth, maxHeight, lut};
  if (!dec.openRAM(buf, (int)size, drawMCU)) return false;

  const int srcW = dec.getWidth(), srcH = dec.getHeight();

  // Safety net for headers the pre-probe could not parse: never run the
  // clipping JPEGDEC path on a larger-than-target source. It crops instead
  // of fitting and faults on such inputs, so hand off to the stb_image
  // fit-to-screen path while the decoder handle is still clean.
  if (maxWidth > 0 && maxHeight > 0 &&
      (srcW > maxWidth || srcH > maxHeight)) {
    dec.close();
    return decodeLargeFitToPage(spr, buf, size, x, y, maxWidth, maxHeight,
                                lut);
  }

  // Pick the cheapest power-of-two scale that still covers the target.
  // The old code always ran a full-resolution decode and then had drawMCU
  // throw away everything past 540x960, so an oversized page (any CBZ that
  // was not pre-fitted) paid for 4x the IDCTs it could display.
  int scale = 0, shift = 0;  // scale: 0 = 1:1
  if (maxWidth > 0 && maxHeight > 0) {
    if (srcW >= maxWidth * 8 && srcH >= maxHeight * 8) {
      scale = JPEG_SCALE_EIGHTH; shift = 3;
    } else if (srcW >= maxWidth * 4 && srcH >= maxHeight * 4) {
      scale = JPEG_SCALE_QUARTER; shift = 2;
    } else if (srcW >= maxWidth * 2 && srcH >= maxHeight * 2) {
      scale = JPEG_SCALE_HALF; shift = 1;
    }
  }

  // fillScreen() is a ~1 MB PSRAM memset. Only the region the page will not
  // cover actually needs it.
  const int outW = srcW >> shift, outH = srcH >> shift;
  if (outW < spr.width() || outH < spr.height()) spr.fillScreen(TFT_WHITE);

  // EIGHT_BIT_GRAYSCALE makes JPEGDEC huffman-skip the Cb/Cr blocks instead
  // of dequantizing and IDCT-ing them (2 of 6 blocks per 4:2:0 MCU), drops
  // the YCbCr->RGB565 conversion entirely, and fits twice as many MCUs per
  // callback. The panel is 16-level gray, so none of that work was visible.
  dec.setPixelType(EIGHT_BIT_GRAYSCALE);
  dec.setUserPointer(&ctx);
  const bool ok = dec.decode(0, 0, JPEG_LUMA_ONLY | scale) != 0;
  dec.close();
  return ok;
}

// JPEG folds contrast/gray into the MCU callback; PNG goes through M5GFX's
// decoder (no callback to hook), so it keeps the old full-frame passes.
// Images larger than the target box in either dimension take the stb_image
// fit-to-screen path: the fast paths clip instead of scaling, and their
// scalers fault on such inputs (e.g. 667x1200 on a 540x960 panel).
static bool decodeImageToSprite(LGFX_Sprite &spr, JPEGDEC &dec, uint8_t *buf,
                                size_t size, int x, int y, int maxWidth,
                                int maxHeight,
                                ContrastPreset preset = CONTRAST_NORMAL,
                                int levels = 16) {
  if (!buf || size == 0) return false;
  if (needsFitToScreen(buf, size, maxWidth, maxHeight)) {
    PixelLut lut;
    buildPixelLut(lut, preset, levels);
    return decodeLargeFitToPage(spr, buf, size, x, y, maxWidth, maxHeight,
                                &lut);
  }
  if (isJpeg(buf, size)) {  // JPEG (clears only the uncovered margins)
    PixelLut lut;
    buildPixelLut(lut, preset, levels);
    return decodeJpegToSprite(dec, spr, buf, size, x, y, maxWidth, maxHeight,
                              &lut);
  }
  spr.fillScreen(TFT_WHITE);
  if (isPng(buf, size)) {  // PNG
    if (!spr.drawPng(buf, (uint32_t)size, x, y, maxWidth, maxHeight))
      return false;
    PixelLut lut;
    buildPixelLut(lut, preset, levels);
    applyPixelLut(spr, lut);  // one 8-bit pass, was two 16-bit ones
    return true;
  }
  return false;
}

static bool forceFullMenuRedraw = true;

// 1:1 blit of an ICON_SIZE grayscale icon (0=black, 255=white) with alpha
// mask into an 8-bit sprite, blended over whatever is already there (the
// card background) so no white box shows around the glyph.
static void blitGrayIcon(LGFX_Sprite &dst, int x, int y,
                         const unsigned char *px,
                         const unsigned char *alpha) {
  uint8_t *buf = (uint8_t *)dst.getBuffer();
  if (!buf) return;
  for (int r = 0; r < ICON_SIZE; r++) {
    uint8_t *row = buf + (y + r) * dst.width() + x;
    const unsigned char *srow = px + r * ICON_SIZE;
    const unsigned char *arow = alpha + r * ICON_SIZE;
    for (int c = 0; c < ICON_SIZE; c++) {
      uint8_t a = arow[c];
      if (a == 255)
        row[c] = srow[c];
      else if (a != 0)
        row[c] = (uint8_t)((srow[c] * a + row[c] * (255 - a) + 127) / 255);
    }
  }
}

void prepareSprite(LGFX_Sprite &sprite, int w, int h,
                   lgfx::v1::color_depth_t depth, bool usePsram) {
  if (sprite.width() == w && sprite.height() == h &&
      sprite.getColorDepth() == depth) {
    return;
  }
  sprite.deleteSprite();
  sprite.setPsram(usePsram);
  sprite.setColorDepth(depth);
  sprite.createSprite(w, h);
}

// applyContrast()/applyGrayLevels() are gone: they walked the sprite as
// uint16_t and would silently corrupt an 8-bit page. buildPixelLut()
// carries their exact behaviour now.
const char *contrastPresetName() {
  switch (contrastPreset) {
    case CONTRAST_NORMAL:
      return "NORMAL";
    case CONTRAST_VIVID:
      return "VIVID";
    case CONTRAST_HIGH:
      return "HIGH";
    case CONTRAST_LIGHT:
      return "LIGHT";
    default:
      return "UNKNOWN";
  }
}

void drawModernButton(LGFX_Sprite &sprite, int x, int y, int w, int h,
                      const char *text, bool isPrimary) {
  if (isPrimary) {
    sprite.fillRoundRect(x, y, w, h, UI_RADIUS, UI_FG);
    sprite.setTextColor(UI_BG, UI_FG);
  } else {
    sprite.fillRoundRect(x, y, w, h, UI_RADIUS, UI_BG);
    sprite.drawRoundRect(x, y, w, h, UI_RADIUS, UI_BORDER);
    sprite.setTextColor(UI_FG, UI_BG);
  }
  sprite.setFont(&fonts::DejaVu18);
  sprite.setTextDatum(middle_center);
  sprite.drawString(text, x + w / 2, y + h / 2);
  sprite.setTextDatum(top_left);
}

// Full-screen pinch-zoom view. Renders the zoomFactor-scaled page region
// around (zoomCX, zoomCY) from gSprite (never modified here). Fastest while
// gliding, quality once the finger stops.
void clampZoomViewport() {
  float vw = (float)DISPLAY_W / zoomFactor;
  float vh = (float)DISPLAY_H / zoomFactor;
  float minX = vw / 2, maxX = (float)DISPLAY_W - vw / 2;
  float minY = vh / 2, maxY = (float)DISPLAY_H - vh / 2;
  float cx = (minX > maxX) ? (float)DISPLAY_W / 2
                           : std::min(std::max((float)zoomCX, minX), maxX);
  float cy = (minY > maxY) ? (float)DISPLAY_H / 2
                           : std::min(std::max((float)zoomCY, minY), maxY);
  zoomCX = (int)cx;
  zoomCY = (int)cy;
}

// Source-resolution zoom for oversized pages. drawZoomed() magnifies gSprite,
// which for an oversized page is already downscaled to fit — zooming it just
// enlarges those pixels. This re-decodes the page at full resolution and
// resamples the zoom viewport straight from the source, so the settled
// quality pass shows true native detail. Returns false on any failure
// (caller falls back to sprite magnification). Only oversized pages take
// this path: 1:1 pages are already exact in gSprite, so re-decoding them
// would only burn time and PSRAM.
static bool renderZoomFromSource() {
  const float z = zoomFactor;
  if (z <= 1.0f) return false;
  clampZoomViewport();
  const int cx = zoomCX, cy = zoomCY;

  uint8_t *zdst = (uint8_t *)s_zoomSprite.getBuffer();
  if (!zdst) return false;

  PageData pg = loadPageData(currentMangaPath, currentPage);
  if (!pg.buf || pg.size == 0 || pg.size > (size_t)INT32_MAX) {
    freePageData(pg);
    return false;
  }
  if (!isOversizedPage(pg.buf, pg.size)) {
    freePageData(pg);
    return false;
  }
  int srcW = 0, srcH = 0;
  if (!imageDimensions(pg.buf, pg.size, &srcW, &srcH)) {
    freePageData(pg);
    return false;
  }
  // Page-fit transform, recomputed identically to decodeLargeFitToPage
  // (oversized guarantees fitScale < 1, so no upscale cap applies).
  const float s =
      std::min((float)DISPLAY_W / (float)srcW, (float)DISPLAY_H / (float)srcH);
  const int outW = (int)((float)srcW * s + 0.5f);
  const int outH = (int)((float)srcH * s + 0.5f);
  const float ox = ((float)DISPLAY_W - (float)outW) / 2.0f;
  const float oy = ((float)DISPLAY_H - (float)outH) / 2.0f;

  // The full frame needs w*h contiguous PSRAM bytes; check before paying
  // for the decode. Worker is idle in large-book mode, so this peak
  // matches a normal large page decode.
  {
    const size_t need = (size_t)srcW * (size_t)srcH;
    if (need > heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)) {
      ESP_LOGW(TAG, "zoom source %dx%d needs %u contig", srcW, srcH,
               (unsigned)need);
      freePageData(pg);
      return false;
    }
  }

  int dw = 0, dh = 0;
  uint8_t *src =
      stbi_load_from_memory(pg.buf, (int)pg.size, &dw, &dh, nullptr, 1);
  freePageData(pg);
  if (!src || dw <= 0 || dh <= 0) {
    if (src) stbi_image_free(src);
    return false;
  }
  srcW = dw;
  srcH = dh;

  PixelLut lut;
  buildPixelLut(lut, contrastPreset, grayLevels);
  const uint8_t *map = lut.map;

  // Output pixel (dx,dy) <- sprite (cx + (dx-270)/z, cy + (dy-480)/z),
  // exactly mirroring pushRotateZoom with pivot (cx,cy) <- source
  // ((sprX-ox)/s, (sprY-oy)/s), the inverse of the page fit. Areas
  // mapping outside the source stay white (pre-filled).
  s_zoomSprite.fillScreen(TFT_WHITE);
  const float invZ = 1.0f / z;
  const float invS = 1.0f / s;
  const float ctrX = (float)DISPLAY_W / 2.0f;
  const float ctrY = (float)DISPLAY_H / 2.0f;
  for (int dy = 0; dy < DISPLAY_H; dy++) {
    const float srcYf = ((float)cy + ((float)dy - ctrY) * invZ - oy) * invS;
    if (srcYf < 0.0f || srcYf > (float)(srcH - 1)) continue;
    int y0 = (int)srcYf;
    float fy = srcYf - (float)y0;
    if (y0 >= srcH - 1) {
      y0 = srcH - 1;
      fy = 0.0f;
    }
    const int y1 = (y0 + 1 < srcH) ? y0 + 1 : y0;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)srcW;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)srcW;
    uint8_t *drow = zdst + (size_t)dy * (size_t)DISPLAY_W;
    for (int dx = 0; dx < DISPLAY_W; dx++) {
      const float srcXf = ((float)cx + ((float)dx - ctrX) * invZ - ox) * invS;
      if (srcXf < 0.0f || srcXf > (float)(srcW - 1)) continue;
      int x0 = (int)srcXf;
      float fx = srcXf - (float)x0;
      if (x0 >= srcW - 1) {
        x0 = srcW - 1;
        fx = 0.0f;
      }
      const int x1 = (x0 + 1 < srcW) ? x0 + 1 : x0;
      const int p00 = row0[x0], p10 = row0[x1];
      const int p01 = row1[x0], p11 = row1[x1];
      const float top = (float)p00 + (float)(p10 - p00) * fx;
      const float bot = (float)p01 + (float)(p11 - p01) * fx;
      int v = (int)(top + (bot - top) * fy + 0.5f);
      if (v < 0)
        v = 0;
      else if (v > 255)
        v = 255;
      drow[dx] = map[v];
    }
  }
  ESP_LOGI(TAG, "zoom source %dx%d z=%.2f", srcW, srcH, z);
  stbi_image_free(src);
  return true;
}

void drawZoomed(bool qualityMode) {
  prepareSprite(s_zoomSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
  if (!s_zoomSprite.getBuffer() || !gSprite.getBuffer()) return;

  // Settled quality pass on an oversized page: render from the full-res
  // source instead of magnifying the fitted sprite. Any failure (normal
  // page, OOM, bad file) falls through to sprite magnification.
  const bool sourced =
      qualityMode && renderZoomFromSource();
  if (!sourced) {
    gSprite.setPivot(zoomCX, zoomCY);
    if (qualityMode)
      gSprite.pushRotateZoomWithAA(&s_zoomSprite, DISPLAY_W / 2,
                                   DISPLAY_H / 2, 0, zoomFactor, zoomFactor);
    else
      gSprite.pushRotateZoom(&s_zoomSprite, DISPLAY_W / 2, DISPLAY_H / 2, 0,
                             zoomFactor, zoomFactor);
  }

  M5.Display.startWrite();
  M5.Display.setEpdMode(qualityMode ? epd_mode_t::epd_quality
                                    : epd_mode_t::epd_fastest);
  s_zoomSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();
}

// Cover thumbnail with a raw-pixel disk cache (see thumb.h). Returns true
// when the frame was painted (caller prints NO COVER otherwise).
// Fast path: validated fread + row memcpy, no decode. Slow path (first
// sighting): decode page 0 fitted into a thumb sprite, persist it, blit.
static bool drawCoverInto(LGFX_Sprite &dst, int x, int y,
                          const std::string &entry) {
  static_assert(THUMB_W - 4 == THUMB_IMG_W && THUMB_H - 4 == THUMB_IMG_H,
                "thumb cache geometry must match the frame inner box");
  uint32_t fsize = 0, fmtime = 0;
  if (!bookCoverSource(entry, fsize, fmtime)) return false;

  const int ox = x + 2, oy = y + 2;  // frame inner top-left
  const std::string key = thumb_key_for(entry, fsize, fmtime);
  uint8_t *dstBuf = (uint8_t *)dst.getBuffer();
  if (!dstBuf) return false;

  uint8_t *raw = (uint8_t *)heap_caps_malloc((size_t)THUMB_IMG_W * THUMB_IMG_H,
                                             MALLOC_CAP_SPIRAM);
  if (raw) {
    if (thumb_load(key.c_str(), raw, THUMB_IMG_W, THUMB_IMG_H)) {
      for (int r = 0; r < THUMB_IMG_H; r++)
        memcpy(dstBuf + (oy + r) * dst.width() + ox, raw + r * THUMB_IMG_W,
               THUMB_IMG_W);
      heap_caps_free(raw);
      return true;
    }
    heap_caps_free(raw);
  }

  // Miss: decode page 0 straight to thumbnail scale (M5GFX picks a small
  // JPEGDIV, so this is cheaper than a full-res decode), persist, blit.
  // Pages larger than the display take the stb_image fit path instead:
  // M5GFX's scaler faults on such sources, just as the page path does.
  static LGFX_Sprite thumbSprite(&M5.Display);
  prepareSprite(thumbSprite, THUMB_IMG_W, THUMB_IMG_H, UI_DEPTH, true);
  if (!thumbSprite.getBuffer()) return false;
  PageData pg = loadPageData(std::string(MANGA_ROOT) + "/" + entry, 0);
  if (!pg.buf) return false;
  thumbSprite.fillScreen(TFT_WHITE);
  bool ok;
  if (needsFitToScreen(pg.buf, pg.size, DISPLAY_W, DISPLAY_H)) {
    ok = decodeLargeFitToThumb(thumbSprite, pg.buf, pg.size);
  } else if (isPng(pg.buf, pg.size))
    ok = thumbSprite.drawPng(pg.buf, (uint32_t)pg.size, 0, 0, THUMB_IMG_W,
                             THUMB_IMG_H, 0, 0, 0.0f, 0.0f, middle_center);
  else
    ok = thumbSprite.drawJpg(pg.buf, (uint32_t)pg.size, 0, 0, THUMB_IMG_W,
                             THUMB_IMG_H, 0, 0, 0.0f, 0.0f, middle_center);
  freePageData(pg);
  if (!ok) return false;
  if (thumb_save(key.c_str(), (const uint8_t *)thumbSprite.getBuffer(),
                 THUMB_IMG_W, THUMB_IMG_H))
    thumb_maintain_cap();
  const uint8_t *src = (const uint8_t *)thumbSprite.getBuffer();
  for (int r = 0; r < THUMB_IMG_H; r++)
    memcpy(dstBuf + (oy + r) * dst.width() + ox, src + r * THUMB_IMG_W,
           THUMB_IMG_W);
  return true;
}

// ---- Background page preloader -------------------------------------------
// Decoding a page takes hundreds of ms; doing it on the main
// task blacked out touch input between page turns. A worker task decodes
// the *next* page into nextPageSprite while the main loop keeps polling
// touch. Protocol (mutex held only for flag checks, never across decode):
// the worker decodes only the latest request and publishes it only if the
// request (path/page/settings) is still current; the main task consumes
// only a matching ready page. Decode buffers and the archive handle below
// are worker-private; nextPageSprite is never touched by both tasks at
// once under this protocol.
struct PreloadReq {
  std::string path;
  int page = -1;
  uint32_t seq = 0;
  ContrastPreset contrast = CONTRAST_NORMAL;
  int gray = 16;
};

static SemaphoreHandle_t s_preMutex = nullptr;
static TaskHandle_t s_preTask = nullptr;
static PreloadReq s_req;
static uint32_t s_reqSeq = 0;
static bool s_preFallbackSync = false;

static CbzArchive *s_preCbz = nullptr;  // worker-private archive handle
static std::string s_preCbzPath;

static bool ensurePreloader();

// Worker-private file read (the shared loadFileToJpgBuffer belongs to the
// main task). Returns bytes read into a fresh PSRAM buffer, 0 on failure.
static size_t preloadReadFile(const char *path, uint8_t **out) {
  *out = nullptr;
  struct stat st = {};
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) return 0;

  const size_t sz = (size_t)st.st_size;
  uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64, (sz + 63) & ~(size_t)63,
                                                    MALLOC_CAP_SPIRAM);
  if (!buf) return 0;
  if (readFileToBuffer(path, buf, sz) != sz) {
    heap_caps_free(buf);
    return 0;
  }
  *out = buf;
  return sz;
}

// ---- Oversized-book memory policy --------------------------------------
// A large page needs its JPEG bytes plus a full w*h grayscale frame in
// PSRAM at once (667x1200 ~= 0.8MB, bigger panoramas multi-MB) on top of
// the ~1.5MB of page sprites. Decoding one concurrently on the worker
// doubles that transient peak, and after some pages of malloc/free churn
// the next contiguous stb allocation fails with "Cannot decode image".
// So once the reader has shown one oversized page, preloading is disabled
// for the rest of that book: every later page decodes on demand on the
// main task with the worker idle. Main-task only state (preloadPage and
// drawPage both run there), reset on book change.
static bool s_largeBookMode = false;
static std::string s_largeBookPath;

static void resetLargeBookModeFor(const std::string &path) {
  if (s_largeBookPath != path) {
    s_largeBookPath = path;
    s_largeBookMode = false;
  }
}

// Cancel any pending/in-flight preload and reclaim the ~0.5MB menu sprite
// (dead weight in reader mode; rebuilt on the next menu visit) before a
// large stb decode. Never touches nextPageSprite: the worker may be
// mid-write into it.
static void prepareForLargeDecode() {
  if (s_preTask && s_preMutex) {
    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    isNextPageReady = false;
    preloadedPage = -1;
    s_reqSeq++;  // in-flight worker result is discarded, never published
    xSemaphoreGive(s_preMutex);
  } else {
    isNextPageReady = false;
    preloadedPage = -1;
  }
  if (menuCacheSprite.getBuffer()) {
    menuCacheSprite.deleteSprite();
    menuCacheValid = false;
  }
  ESP_LOGI(TAG, "large-decode PSRAM free=%u largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void preloadPage(int page) {
  resetLargeBookModeFor(currentMangaPath);
  if (s_largeBookMode) return;  // oversized book: decode on demand, save PSRAM
  if (!ensurePreloader()) return;  // worker unavailable: skip preloading
  if (page < 0 || page >= totalPages) return;

  xSemaphoreTake(s_preMutex, portMAX_DELAY);
  const bool sameReady = isNextPageReady && preloadedPage == page &&
                         preloadedMangaPath == currentMangaPath;
  const bool inFlight =
      (s_req.path == currentMangaPath && s_req.page == page);
  if (!sameReady && !inFlight) {
    // The worker is about to overwrite nextPageSprite, so whatever was
    // published for it is no longer safe to hand to the display. Matters
    // more now that drawPage() swaps that sprite in rather than copying it.
    isNextPageReady = false;
    s_req.path = currentMangaPath;
    s_req.page = page;
    s_req.seq = ++s_reqSeq;
    s_req.contrast = contrastPreset;
    s_req.gray = grayLevels;
    xSemaphoreGive(s_preMutex);
    xTaskNotifyGive(s_preTask);
  } else {
    xSemaphoreGive(s_preMutex);
  }
}

// Decode one page into nextPageSprite on the worker task. Uses only
// worker-private buffers and its own CBZ handle; the handoff globals are
// published under s_preMutex only if the request is still current.
static void preloadDecode(const PreloadReq &req) {
  if (req.page < 0) {
    // Book-switch cleanup request: drop the worker archive when it no
    // longer matches, so the old book's central directory doesn't pin
    // PSRAM forever (no further preloads arrive in large-book mode).
    if (s_preCbz && s_preCbzPath != req.path) {
      cbz_close(s_preCbz);
      s_preCbz = nullptr;
      s_preCbzPath = "";
    }
    return;
  }

  // The worker never went through main.cpp's setCpuFrequencyMhz() brackets,
  // so its decodes ran at the DFS minimum.
  CpuBoost boost;
  const int64_t tStart = esp_timer_get_time();

  prepareSprite(nextPageSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
  if (!nextPageSprite.getBuffer()) return;

  uint8_t *buf = nullptr;
  size_t size = 0;
  if (isCbzPath(req.path)) {
    if (!s_preCbz || s_preCbzPath != req.path) {
      cbz_close(s_preCbz);
      s_preCbz = cbz_open(req.path);
      s_preCbzPath = s_preCbz ? req.path : std::string();
    }
    if (s_preCbz) size = cbz_extract(s_preCbz, (size_t)req.page, &buf);
  } else {
    size = preloadReadFile(makePagePath(req.path, req.page).c_str(), &buf);
  }

  // Oversized target: skip the multi-MB stb frame entirely. The main task
  // decodes it on demand with the worker idle (see policy above). Probes
  // confirmed dimensions only, so small progressive pages still preload.
  if (buf && size > 0 && isOversizedPage(buf, size)) {
    ESP_LOGI(TAG, "preload %d: oversized, skip (main will decode)", req.page);
    heap_caps_free(buf);
    return;
  }

  const int64_t tDec0 = esp_timer_get_time();
  bool ok = (buf != nullptr && size > 0) &&
            decodeImageToSprite(nextPageSprite, s_jpegWorker, buf, size, 0,
                                0, DISPLAY_W, DISPLAY_H, req.contrast,
                                req.gray);
  if (buf) heap_caps_free(buf);
  if (!ok) return;
  ESP_LOGI(TAG, "preload %d: read %d ms, decode %d ms", req.page,
           (int)((tDec0 - tStart) / 1000),
           (int)((esp_timer_get_time() - tDec0) / 1000));

  xSemaphoreTake(s_preMutex, portMAX_DELAY);
  if (req.seq == s_reqSeq && req.path == s_req.path &&
      contrastPreset == req.contrast && grayLevels == req.gray) {
    preloadedPage = req.page;
    preloadedMangaPath = req.path;
    isNextPageReady = true;
  }
  xSemaphoreGive(s_preMutex);
}

static void preloadTaskFn(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // coalesces repeat requests
    PreloadReq req;
    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    req = s_req;
    xSemaphoreGive(s_preMutex);
    preloadDecode(req);
  }
}

static bool ensurePreloader() {
  if (s_preTask) return true;
  if (s_preFallbackSync) return false;
  s_preMutex = xSemaphoreCreateMutex();
  if (!s_preMutex) {
    s_preFallbackSync = true;
    return false;
  }
  // Same core + priority as the main task: round-robin keeps touch
  // polling live, and the EPD writer core stays undisturbed.
  if (xTaskCreatePinnedToCore(preloadTaskFn, "preload", 12288, nullptr, 1,
                              &s_preTask, xPortGetCoreID()) != pdPASS) {
    s_preTask = nullptr;
    s_preFallbackSync = true;
    return false;
  }
  return true;
}

void drawMenu() {
  static std::string drawnLastMangaName = "";
  static int drawnLastPage = -1;
  static int drawnLastMenuScroll = -1;

  int totalItems = (int)mangaFolders.size() + 2;
  int end = std::min(totalItems, menuScroll + MENU_VISIBLE);

  if (!menuCacheValid || lastDrawnMenuScroll != menuScroll) {
    prepareSprite(menuCacheSprite, DISPLAY_W, DISPLAY_H, UI_DEPTH, true);
    if (menuCacheSprite.getBuffer()) {
      menuCacheSprite.fillScreen(UI_BG);
      // Black header bar, white text (its bottom edge is the divider).
      menuCacheSprite.fillRect(0, 0, DISPLAY_W, 80, UI_FG);
      menuCacheSprite.setFont(&fonts::DejaVu24);
      menuCacheSprite.setTextColor(UI_BG, UI_FG);
      menuCacheSprite.setCursor(GRID_GUTTER, 19);
      menuCacheSprite.print("Library");

      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setTextColor(UI_BG, UI_FG);
      menuCacheSprite.setCursor(GRID_GUTTER, 49);
      menuCacheSprite.printf("%d titles available", (int)mangaFolders.size());

      if (mangaFolders.empty()) {
        menuCacheSprite.setFont(&fonts::DejaVu18);
        menuCacheSprite.setTextColor(TFT_BLACK, TFT_WHITE);
        menuCacheSprite.setCursor(GRID_GUTTER, 120);
        menuCacheSprite.println("No manga found in /manga/");
      } else {
        for (int i = menuScroll; i < end; i++) {
          int relIdx = i - menuScroll;
          int row = relIdx / GRID_COLS;
          int col = relIdx % GRID_COLS;
          int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
          int y = GRID_Y_TOP + row * GRID_ROW_H;

          menuCacheSprite.fillRoundRect(x, y, THUMB_W, THUMB_H, UI_RADIUS,
                                        UI_BG);
          menuCacheSprite.drawRoundRect(x, y, THUMB_W, THUMB_H, UI_RADIUS,
                                        UI_BORDER);

          if (i == 0) {
            menuCacheSprite.fillRoundRect(x + 10, y + 10, THUMB_W - 20,
                                          THUMB_H - 20, UI_RADIUS, UI_ACCENT);
            blitGrayIcon(menuCacheSprite, x + (THUMB_W - ICON_SIZE) / 2,
                         y + (THUMB_H - ICON_SIZE) / 2, icon_bookmarks_128,
                         icon_bookmarks_128_alpha);
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString("BOOKMARKS", x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          } else if (i == 1) {
            menuCacheSprite.fillRoundRect(x + 10, y + 10, THUMB_W - 20,
                                          THUMB_H - 20, UI_RADIUS, UI_ACCENT);
            blitGrayIcon(menuCacheSprite, x + (THUMB_W - ICON_SIZE) / 2,
                         y + (THUMB_H - ICON_SIZE) / 2, icon_files_128,
                         icon_files_128_alpha);
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString("FILES", x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          } else {
            int fIdx = i - 2;
            if (!drawCoverInto(menuCacheSprite, x, y, mangaFolders[fIdx])) {
              menuCacheSprite.setTextColor(UI_FG, UI_BG);
              menuCacheSprite.setFont(&fonts::DejaVu12);
              menuCacheSprite.setCursor(x + 10, y + THUMB_H / 2);
              menuCacheSprite.print("NO COVER");
            }
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            std::string title = displayName(mangaFolders[fIdx]);
            if (title.length() > 22) title = title.substr(0, 20) + "...";

            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString(title.c_str(), x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          }
        }
      }
      menuCacheValid = true;
      drawnLastPage = -1;
      lastDrawnMenuScroll = menuScroll;
      forceFullMenuRedraw = true;
    }
  }

  if (menuCacheValid &&
      (drawnLastMangaName != lastMangaName || drawnLastPage != lastPage ||
       drawnLastMenuScroll != menuScroll)) {
    int barW = DISPLAY_W - 40;
    int barH = 60;
    int barX = 20;
    int barY = DISPLAY_H - 85;

    if (lastMangaName.length() == 0) {
      menuCacheSprite.fillRect(barX, barY - 2, barW + 4, barH + 4, UI_BG);
    } else {
      menuCacheSprite.fillRoundRect(barX + 2, barY + 2, barW, barH, UI_RADIUS,
                                    UI_SHADOW);
      menuCacheSprite.fillRoundRect(barX, barY, barW, barH, UI_RADIUS, UI_BG);
      menuCacheSprite.drawRoundRect(barX, barY, barW, barH, UI_RADIUS,
                                    UI_BORDER);

      menuCacheSprite.setTextColor(UI_FG, UI_BG);
      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setCursor(barX + 15, barY + 12);
      menuCacheSprite.print("CONTINUE: ");

      menuCacheSprite.setFont(&fonts::DejaVu18);
      std::string shortName = displayName(lastMangaName);
      if (shortName.length() > 19) shortName = shortName.substr(0, 17) + "...";
      menuCacheSprite.print(shortName.c_str());

      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setCursor(barX + 15, barY + 36);
      menuCacheSprite.printf("Page %d", lastPage + 1);

      int btnW = 90;
      int btnH = 40;
      int btnX = barX + barW - btnW - 10;
      int btnY = barY + 10;
      drawModernButton(menuCacheSprite, btnX, btnY, btnW, btnH, "RESUME", true);
    }
    drawnLastMangaName = lastMangaName;
    drawnLastPage = lastPage;
    drawnLastMenuScroll = menuScroll;
    forceFullMenuRedraw = true;
  }

  static int prevMenuSelected = -1;
  static int lastMenuScrollForDirty = -1;
  bool isFullRedraw = forceFullMenuRedraw;
  if (lastMenuScrollForDirty != menuScroll) isFullRedraw = true;
  lastMenuScrollForDirty = menuScroll;
  forceFullMenuRedraw = false;

  M5.Display.startWrite();
  if (isFullRedraw) {
    if (menuCacheValid)
      menuCacheSprite.pushSprite(0, 0);
    else
      M5.Display.fillScreen(TFT_WHITE);
    for (int i = menuScroll; i < end; i++) {
      if (i == menuSelected) {
        int relIdx = i - menuScroll;
        int row = relIdx / GRID_COLS;
        int col = relIdx % GRID_COLS;
        int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
        int y = GRID_Y_TOP + row * GRID_ROW_H;
        M5.Display.drawRoundRect(x - 5, y - 5, THUMB_W + 10, THUMB_H + 10,
                                 UI_RADIUS + 2, TFT_BLACK);
        M5.Display.drawRoundRect(x - 4, y - 4, THUMB_W + 8, THUMB_H + 8,
                                 UI_RADIUS + 1, TFT_BLACK);
        M5.Display.fillRect(x + (THUMB_W / 2) - 15, y + THUMB_H + 30, 30, 3,
                            TFT_BLACK);
      }
    }
  } else {
    if (prevMenuSelected >= menuScroll && prevMenuSelected < end) {
      int relIdx = prevMenuSelected - menuScroll;
      int row = relIdx / GRID_COLS;
      int col = relIdx % GRID_COLS;
      int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
      int y = GRID_Y_TOP + row * GRID_ROW_H;
      M5.Display.setClipRect(x - 6, y - 6, THUMB_W + 12, THUMB_H + 40);
      menuCacheSprite.pushSprite(0, 0);
      M5.Display.clearClipRect();
    }
    if (menuSelected >= menuScroll && menuSelected < end) {
      int relIdx = menuSelected - menuScroll;
      int row = relIdx / GRID_COLS;
      int col = relIdx % GRID_COLS;
      int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
      int y = GRID_Y_TOP + row * GRID_ROW_H;
      M5.Display.setClipRect(x - 6, y - 6, THUMB_W + 12, THUMB_H + 40);
      menuCacheSprite.pushSprite(0, 0);
      M5.Display.clearClipRect();
      M5.Display.drawRoundRect(x - 5, y - 5, THUMB_W + 10, THUMB_H + 10,
                               UI_RADIUS + 2, TFT_BLACK);
      M5.Display.drawRoundRect(x - 4, y - 4, THUMB_W + 8, THUMB_H + 8,
                               UI_RADIUS + 1, TFT_BLACK);
      M5.Display.fillRect(x + (THUMB_W / 2) - 15, y + THUMB_H + 30, 30, 3,
                          TFT_BLACK);
    }
  }
  prevMenuSelected = menuSelected;
  M5.Display.display();
  M5.Display.endWrite();
}

void drawControlCenter() {
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, UI_DEPTH, true);
  if (!gSprite.getBuffer()) return;
  gSprite.fillScreen(TFT_MAGENTA);
  int modW = 500;
  int modH = 300;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  gSprite.fillRoundRect(modX + 6, modY + 6, modW, modH, UI_RADIUS, UI_SHADOW);

  gSprite.fillRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BG);
  gSprite.drawRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BORDER);
  gSprite.drawRoundRect(modX + 1, modY + 1, modW - 2, modH - 2, UI_RADIUS - 1,
                        UI_BORDER);
  gSprite.drawRoundRect(modX + 2, modY + 2, modW - 4, modH - 4, UI_RADIUS - 2,
                        UI_BORDER);

  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setTextDatum(top_center);
  gSprite.drawString("System Menu", modX + modW / 2, modY + 25);
  gSprite.setTextDatum(top_left);
  gSprite.drawLine(modX, modY + 70, modX + modW, modY + 70, UI_BORDER);

  int bat = M5.Power.getBatteryLevel();
  gSprite.setFont(&fonts::DejaVu18);
  gSprite.setCursor(modX + 30, modY + 100);
  gSprite.printf("Battery Level: %d%%", bat);

  int pW = modW - 60;
  int pX = modX + 30;
  int pY = modY + 130;
  gSprite.drawRoundRect(pX, pY, pW, 40, UI_RADIUS, UI_BORDER);
  gSprite.fillRoundRect(pX + 4, pY + 4, (pW - 8) * bat / 100, 32,
                        UI_RADIUS - 2, UI_FG);

  int btnW = modW - 60;
  int btnH = 60;
  int btnX = modX + 30;
  int btnY = modY + 210;
  drawModernButton(gSprite, btnX, btnY, btnW, btnH, "SHUTDOWN", true);

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0, TFT_MAGENTA);
  M5.Display.display();
  M5.Display.endWrite();
}

void fullRefresh() {
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  // PaperS3-only firmware: the panel is always the direct-drive EPD.
  if (M5.Display.getBoard() == lgfx::board_M5PaperS3) {
    auto *panel = static_cast<lgfx::Panel_EPD *>(M5.Display.getPanel());
    if (panel) panel->refreshStockWaveform();
  }
  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();
  M5.Display.waitDisplay();
}

void systemShutdown() {
  flushProgress();  // powerOff() below never returns; persist a throttled save
  setCpuFrequencyMhz(240);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  std::vector<std::string> pics;
  DIR *root = opendir(PIC_ROOT);
  if (root) {
    struct dirent *entry;
    while ((entry = readdir(root)) != nullptr) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (str_ends_with(name, ".jpg") || str_ends_with(name, ".jpeg") ||
          str_ends_with(name, ".JPG") || str_ends_with(name, ".JPEG")) {
        pics.push_back(name);
      }
    }
    closedir(root);
  }
  bool haveImage = false;
  if (!pics.empty()) {
    size_t r = esp_random() % pics.size();
    std::string path = pics[r];
    if (path.empty() || path[0] != '/')
      path = std::string(PIC_ROOT) + "/" + path;
    else if (!str_starts_with(path, "/sdcard"))
      path = std::string(PIC_ROOT) + path;
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
    if (gSprite.getBuffer()) {
      size_t sz = loadFileToJpgBuffer(path.c_str());
      if (sz > 0 &&
          decodeImageToSprite(gSprite, s_jpegMain, jpgSharedBuffer(), sz, 0,
                              0, DISPLAY_W, DISPLAY_H))
        haveImage = true;
    }
  }
  if (!haveImage) {
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
    if (gSprite.getBuffer()) gSprite.fillScreen(TFT_WHITE);
  }
  if (gSprite.getBuffer())
    fullRefresh();  // stock sequence so the splash persists ghost-free
  else {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.display();
  }
  idf_delay(2000);
  M5.Power.powerOff();
}

void drawBookConfig() {
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, UI_DEPTH, true);
  if (!gSprite.getBuffer()) return;
  gSprite.fillScreen(TFT_MAGENTA);
  int modW = BOOK_MOD_W;
  int modH = BOOK_MOD_H;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  gSprite.fillRoundRect(modX + 6, modY + 6, modW, modH, UI_RADIUS, UI_SHADOW);

  gSprite.fillRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BG);
  gSprite.drawRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BORDER);
  gSprite.drawRoundRect(modX + 1, modY + 1, modW - 2, modH - 2, UI_RADIUS - 1,
                        UI_BORDER);
  gSprite.drawRoundRect(modX + 2, modY + 2, modW - 4, modH - 4, UI_RADIUS - 2,
                        UI_BORDER);

  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setTextDatum(top_center);
  gSprite.drawString("Book Menu", modX + modW / 2, modY + 25);
  gSprite.setTextDatum(top_left);
  gSprite.drawLine(modX, modY + 70, modX + modW, modY + 70, UI_BORDER);

  int barY = modY + BOOK_PAGE_Y;
  gSprite.drawRoundRect(modX + 20, barY, modW - 40, BOOK_PAGE_H, UI_RADIUS,
                        UI_BORDER);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(modX + 45, barY + 22);
  gSprite.print("<<");
  gSprite.setCursor(modX + 115, barY + 22);
  gSprite.print("<");
  std::string pg =
      std::to_string(bookConfigPendingPage + 1) + " / " + std::to_string(totalPages);
  int pgW = (int)pg.length() * 14;
  gSprite.setCursor(modX + (modW - pgW) / 2, barY + 22);
  gSprite.print(pg.c_str());
  gSprite.setCursor(modX + modW - 135, barY + 22);
  gSprite.print(">");
  gSprite.setCursor(modX + modW - 85, barY + 22);
  gSprite.print(">>");

  // Chapter stepper: "< Name (i/n) >" for multi-section CBZ, plain
  // "NO CHAPTERS" otherwise. Jumps move the pending page to the
  // neighboring chapter's first page (applied on close, like pages).
  int chapY = modY + BOOK_CHAP_Y;
  gSprite.drawRoundRect(modX + 20, chapY, modW - 40, BOOK_CHAP_H, UI_RADIUS,
                        UI_BORDER);
  const ChapterList &chapters = getChapters(currentMangaPath);
  int chapIdx = chapterIndexForPage(currentMangaPath, bookConfigPendingPage);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(modX + 45, chapY + 16);
  gSprite.print("<");
  gSprite.setCursor(modX + modW - 75, chapY + 16);
  gSprite.print(">");
  gSprite.setFont(&fonts::DejaVu18);
  gSprite.setTextDatum(middle_center);
  if (chapIdx >= 0) {
    std::string label = chapters.names[(size_t)chapIdx] + " (" +
                        std::to_string(chapIdx + 1) + "/" +
                        std::to_string(chapters.names.size()) + ")";
    if (label.length() > 24) label = label.substr(0, 22) + "...";
    gSprite.drawString(label.c_str(), modX + modW / 2, chapY + BOOK_CHAP_H / 2);
  } else {
    gSprite.drawString("NO CHAPTERS", modX + modW / 2, chapY + BOOK_CHAP_H / 2);
  }
  gSprite.setTextDatum(top_left);

  gSprite.drawLine(modX, modY + BOOK_DIV_Y, modX + modW, modY + BOOK_DIV_Y,
                   UI_BORDER);

  int btnW = BOOK_BTN_W;
  int btnX = modX + BOOK_BTN_XOFF;
  int btnH = BOOK_BTN_H;

  int btnY0 = modY + BOOK_BTN_GRAY_Y;
  std::string grayMsg = std::string("GRAY: ") + std::to_string(grayLevels);
  drawModernButton(gSprite, btnX, btnY0, btnW, btnH, grayMsg.c_str(), false);

  int btnY1 = modY + BOOK_BTN_CONTRAST_Y;
  std::string contrastMsg = std::string("CONTRAST: ") + contrastPresetName();
  drawModernButton(gSprite, btnX, btnY1, btnW, btnH, contrastMsg.c_str(),
                   false);

  int btnY2 = modY + BOOK_BTN_BOOKMARK_Y;
  drawModernButton(gSprite, btnX, btnY2, btnW, btnH, "BOOKMARK PAGE", false);

  int btnY3 = modY + BOOK_BTN_RETURN_Y;
  drawModernButton(gSprite, btnX, btnY3, btnW, btnH, "RETURN TO LIBRARY", true);

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0, TFT_MAGENTA);
  M5.Display.display();
  M5.Display.endWrite();
}

void drawBookmarks() {
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, UI_DEPTH, true);
  if (!gSprite.getBuffer()) return;
  gSprite.fillScreen(UI_BG);
  gSprite.drawLine(0, 80, DISPLAY_W, 80, UI_BORDER);
  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(20, 30);
  if (selectedBookmarkFolder == "")
    gSprite.print("Bookmark Library");
  else {
    std::string t = displayName(selectedBookmarkFolder);
    if (t.length() > 19) t = t.substr(0, 17) + "...";
    gSprite.printf("< %s", t.c_str());
  }
  int itemsPerPage = (DISPLAY_H - 200) / 90;
  int totalItems = 0;
  if (bookmarks.empty()) {
    gSprite.setTextColor(UI_FG, UI_BG);
    gSprite.setFont(&fonts::DejaVu18);
    gSprite.setCursor(40, 150);
    gSprite.print("No bookmarks yet.");
  } else {
    int yOff = 100;
    int itemH = 80;
    if (selectedBookmarkFolder == "") {
      auto uniqueFolders = getUniqueBookmarkFolders();
      totalItems = (int)uniqueFolders.size();
      int count = 0;
      for (int i = 0; i < (int)uniqueFolders.size(); i++) {
        if (i < bookmarkScroll) continue;
        if (count >= itemsPerPage) break;
        gSprite.fillRoundRect(10 + 4, yOff + 4, DISPLAY_W - 20, itemH,
                              UI_RADIUS, UI_SHADOW);
        gSprite.fillRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BG);
        gSprite.drawRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BORDER);
        gSprite.setTextColor(UI_FG, UI_BG);
        gSprite.setFont(&fonts::DejaVu18);
        gSprite.setCursor(30, yOff + 30);
        std::string t = displayName(uniqueFolders[i]);
        if (t.length() > 26) t = t.substr(0, 24) + "...";
        gSprite.print(t.c_str());
        gSprite.setFont(&fonts::DejaVu12);
        gSprite.setTextColor(UI_FG, UI_BG);
        gSprite.setCursor(DISPLAY_W - 80, yOff + 34);
        gSprite.print("OPEN >");
        yOff += itemH + 10;
        count++;
      }
    } else {
      int count = 0;
      int folderItemIdx = 0;
      for (int i = 0; i < (int)bookmarks.size(); i++) {
        if (bookmarks[i].folder != selectedBookmarkFolder) continue;
        totalItems++;
        if (folderItemIdx < bookmarkScroll) {
          folderItemIdx++;
          continue;
        }
        if (count >= itemsPerPage) {
          folderItemIdx++;
          continue;
        }
        gSprite.fillRoundRect(10 + 4, yOff + 4, DISPLAY_W - 20, itemH,
                              UI_RADIUS, UI_SHADOW);
        gSprite.fillRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BG);
        gSprite.drawRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BORDER);
        gSprite.setTextColor(UI_FG, UI_BG);
        gSprite.setFont(&fonts::DejaVu18);
        gSprite.setCursor(30, yOff + 30);
        gSprite.printf("Page %d", bookmarks[i].page + 1);

        drawModernButton(gSprite, DISPLAY_W - 100, yOff + 20, 70, 40, "DEL",
                         true);
        yOff += itemH + 10;
        count++;
        folderItemIdx++;
      }
    }
  }
  if (totalItems > 0) {
    int curPg = (bookmarkScroll / itemsPerPage) + 1;
    int maxPg = (totalItems + itemsPerPage - 1) / itemsPerPage;
    gSprite.setTextColor(UI_FG, UI_BG);
    gSprite.setFont(&fonts::DejaVu12);
    gSprite.setCursor(DISPLAY_W - 100, 30);
    gSprite.printf("Pg %d/%d", curPg, maxPg);
  }

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();
}

void drawWifiServer() {
  forceFullMenuRedraw = true;
  if (!isWifiServerRunning()) startWifiServer();
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, UI_DEPTH, true);
  if (!gSprite.getBuffer()) return;
  gSprite.fillScreen(UI_BG);
  gSprite.drawLine(0, 80, DISPLAY_W, 80, UI_BORDER);
  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(20, 30);
  gSprite.print("WiFi File Browser");
  gSprite.setFont(&fonts::DejaVu24);
  int y = 150;
  gSprite.setCursor(40, y);
  gSprite.print("Access Point Started");
  y += 60;
  gSprite.setFont(&fonts::DejaVu18);
  gSprite.setCursor(40, y);
  gSprite.print("Connect to:");
  y += 40;
  gSprite.setCursor(60, y);
  gSprite.print(getWifiSSID().c_str());
  y += 80;
  gSprite.setCursor(40, y);
  gSprite.print("Open in Browser:");
  y += 40;
  gSprite.setCursor(60, y);
  gSprite.printf("http://%s", getWifiIP().c_str());

  int btnW = 300;
  int btnH = 60;
  int btnX = (DISPLAY_W - btnW) / 2;
  int btnY = DISPLAY_H - 150;
  drawModernButton(gSprite, btnX, btnY, btnW, btnH, "STOP SERVER", true);
  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();
}

// What prevPageSprite currently holds, and what is on screen. Settings are
// part of the key: a contrast or gray-level change makes a kept page stale.
static std::string s_prevPath, s_shownPath;
static int s_prevPage = -1, s_shownPage = -1;
static ContrastPreset s_prevContrast = CONTRAST_NORMAL,
                      s_shownContrast = CONTRAST_NORMAL;
static int s_prevGray = 16, s_shownGray = 16;

static bool prevSlotMatches() {
  return s_prevPage == currentPage && s_prevPath == currentMangaPath &&
         s_prevContrast == contrastPreset && s_prevGray == grayLevels &&
         prevPageSprite.getBuffer() != nullptr &&
         prevPageSprite.getColorDepth() == PAGE_DEPTH;
}

// Book-switch cleanup (called from openMangaPath): leave no stale per-book
// PSRAM behind, so returning to a large book finds a clean heap for its
// w*h stb frame. Cancels preloads, asks the worker to drop its archive
// handle, resets large-book + prev-slot keys, and frees the zoom scratch
// buffer. The main archive, chapter list and shared jpg buffer are dropped
// via dropCachedBookData(); page sprites are intentionally kept (fixed
// size, reused in place by the first drawPage).
void resetReaderForBookSwitch(const std::string &newPath) {
  if (s_preTask && s_preMutex) {
    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    isNextPageReady = false;
    preloadedPage = -1;
    preloadedMangaPath = "";
    // Cleanup request, not a page: the worker drops s_preCbz when stale.
    // An in-flight decode (if any) finishes but is discarded via seq.
    s_req.path = newPath;
    s_req.page = -1;
    s_req.seq = ++s_reqSeq;
    xSemaphoreGive(s_preMutex);
    xTaskNotifyGive(s_preTask);
  } else {
    isNextPageReady = false;
    preloadedPage = -1;
    preloadedMangaPath = "";
  }
  s_largeBookPath = newPath;
  s_largeBookMode = false;
  s_prevPath = "";
  s_prevPage = -1;
  s_shownPath = "";
  s_shownPage = -1;
  if (s_zoomSprite.getBuffer()) s_zoomSprite.deleteSprite();
  ESP_LOGI(TAG, "book switch PSRAM free=%u largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void drawPage() {
  // Held for the whole load: setCpuFrequencyMhz(80) on the early-return
  // paths below would otherwise drop the clock mid-decode.
  CpuBoost boost;

  forceFullMenuRedraw = true;
  if (totalPages == 0) {
    drawError("No images in this manga.");
    return;
  }

  setCpuFrequencyMhz(240);

  bool usePreload = false;
  if (ensurePreloader()) {
    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    if (isNextPageReady && preloadedPage == currentPage &&
        preloadedMangaPath == currentMangaPath) {
      usePreload = true;
    }
    xSemaphoreGive(s_preMutex);
  }

  if (usePreload) {
    ESP_LOGI(TAG, "Instant turn for page %d", currentPage + 1);

    // Pointer rotation, not a full-frame copy. Under the mutex so the worker
    // cannot be mid-publish, and the ready flag is cleared before it can be
    // asked to start again.
    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    rotatePageSprites();
    s_prevPath = s_shownPath;  // the page we just left is now in prevPageSprite
    s_prevPage = s_shownPage;
    s_prevContrast = s_shownContrast;
    s_prevGray = s_shownGray;
    isNextPageReady = false;
    xSemaphoreGive(s_preMutex);
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
  } else if (prevSlotMatches()) {
    // Back-turn onto the page we just left: swap it in, no read, no decode.
    ESP_LOGI(TAG, "Prev-slot hit for page %d", currentPage + 1);
    swapPrevPageSprite();
    s_prevPath = s_shownPath;
    s_prevPage = s_shownPage;
    s_prevContrast = s_shownContrast;
    s_prevGray = s_shownGray;
  } else {
    ESP_LOGI(TAG, "Drawing [%d/%d] page %d", currentPage + 1, totalPages,
             currentPage);

    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, PAGE_DEPTH, true);
    if (!gSprite.getBuffer()) {
      setCpuFrequencyMhz(80);
      drawError("Sprite alloc failed.");
      return;
    }

    const int64_t tRead0 = esp_timer_get_time();
    PageData pg = loadPageData(currentMangaPath, currentPage);
    if (!pg.buf) {
      setCpuFrequencyMhz(80);
      drawError("Cannot open image.");
      return;
    }
    resetLargeBookModeFor(currentMangaPath);
    if (isOversizedPage(pg.buf, pg.size)) {
      s_largeBookMode = true;
      // Reclaim PSRAM before the w*h stb frame is allocated: drop any
      // pending preload and the dead menu cache (see policy above).
      prepareForLargeDecode();
    }
    const int64_t tDec0 = esp_timer_get_time();
    bool decodeSuccess = decodeImageToSprite(gSprite, s_jpegMain, pg.buf,
                                             pg.size, 0, 0, DISPLAY_W,
                                             DISPLAY_H, contrastPreset,
                                             grayLevels);
    const int64_t tDec1 = esp_timer_get_time();
    ESP_LOGI(TAG, "page %d: %u KB, read %d ms, decode %d ms", currentPage,
             (unsigned)(pg.size / 1024), (int)((tDec0 - tRead0) / 1000),
             (int)((tDec1 - tDec0) / 1000));
    freePageData(pg);

    if (!decodeSuccess) {
      setCpuFrequencyMhz(80);
      drawError("Cannot decode image.");
      return;
    }
  }

  // Kick the preloader *before* the refresh, not after. M5.Display.display()
  // spends ~18 waveform scans driving the panel, most of it waiting on EPD
  // timing rather than on the CPU, and the worker used to sit idle through
  // all of it and only start once the user could already see the page.
  //
  // Direction matters too: the old code always preloaded currentPage + 1, so
  // every turn while reading backwards was a guaranteed miss.
  const bool backwards = (s_shownPage >= 0 && currentPage < s_shownPage);
  s_shownPath = currentMangaPath;
  s_shownPage = currentPage;
  s_shownContrast = contrastPreset;
  s_shownGray = grayLevels;
  const int wanted = backwards ? currentPage - 1 : currentPage + 1;
  // Oversized books decode on demand (see policy above): kicking the worker
  // here would only fragment PSRAM ahead of the next large stb frame.
  if (wanted >= 0 && wanted < totalPages && !s_largeBookMode)
    preloadPage(wanted);

  const int64_t tPush0 = esp_timer_get_time();
  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();
  ESP_LOGI(TAG, "page %d: panel %d ms", currentPage,
           (int)((esp_timer_get_time() - tPush0) / 1000));

  setCpuFrequencyMhz(80);
}

void drawError(const char *msg) {
  forceFullMenuRedraw = true;
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(20, 40);
  M5.Display.println(msg);
  M5.Display.display();
}

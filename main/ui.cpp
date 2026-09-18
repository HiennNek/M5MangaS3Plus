#include "ui.h"

#include <dirent.h>

#include <algorithm>
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

// One decoder per task: JPEGDEC keeps decode state in the object, and the
// main task (page render) and the preload worker can decode concurrently.
static JPEGDEC s_jpegMain;
static JPEGDEC s_jpegWorker;

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

struct JpegDrawContext {
  LGFX_Sprite *spr;
  int offsetX;
  int offsetY;
  int maxWidth;
  int maxHeight;
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

  const uint16_t *pixels = pDraw->pPixels;
  uint16_t *dst = (uint16_t *)spr->getBuffer();
  for (int y = 0; y < ch; y++) {
    memcpy(&dst[(outY + y) * sprW + outX], &pixels[y * pDraw->iWidth],
           (size_t)cw * sizeof(uint16_t));
  }
  return 1;
}

static bool decodeJpegToSprite(JPEGDEC &dec, LGFX_Sprite &spr, uint8_t *buf,
                               size_t size, int x, int y, int maxWidth,
                               int maxHeight) {
  JpegDrawContext ctx = {&spr, x, y, maxWidth, maxHeight};
  if (!dec.openRAM(buf, (int)size, drawMCU)) return false;
  dec.setPixelType(RGB565_BIG_ENDIAN);
  dec.setUserPointer(&ctx);
  const bool ok = dec.decode(0, 0, 0) != 0;
  dec.close();
  return ok;
}

static bool decodeImageToSprite(LGFX_Sprite &spr, JPEGDEC &dec, uint8_t *buf,
                                size_t size, int x, int y, int maxWidth,
                                int maxHeight) {
  if (!buf || size == 0) return false;
  spr.fillScreen(TFT_WHITE);
  if (isJpeg(buf, size)) {  // JPEG
    return decodeJpegToSprite(dec, spr, buf, size, x, y, maxWidth, maxHeight);
  }
  if (isPng(buf, size)) {  // PNG
    return spr.drawPng(buf, (uint32_t)size, x, y, maxWidth, maxHeight);
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

void prepareSprite(LGFX_Sprite &sprite, int w, int h, int depth,
                   bool usePsram) {
  if (sprite.width() == w && sprite.height() == h &&
      sprite.getColorDepth() == depth) {
    return;
  }
  sprite.deleteSprite();
  sprite.setPsram(usePsram);
  sprite.setColorDepth(depth);
  sprite.createSprite(w, h);
}

void applyContrast(LGFX_Sprite &sprite, ContrastPreset preset) {
  if (preset == CONTRAST_NORMAL) return;

  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf) return;

  int32_t contrast_fp, brightness;
  switch (preset) {
    case CONTRAST_VIVID:
      contrast_fp = 307;
      brightness = 0;
      break;
    case CONTRAST_HIGH:
      contrast_fp = 358;
      brightness = 5;
      break;
    case CONTRAST_LIGHT:
      contrast_fp = 256;
      brightness = 30;
      break;
    default:
      return;
  }

  uint8_t lut[256];
  for (int i = 0; i < 256; i++) {
    int32_t val = (((i - 128) * contrast_fp) >> 8) + 128 + brightness;
    if (val < 0)
      val = 0;
    else if (val > 255)
      val = 255;
    lut[i] = (uint8_t)val;
  }

  for (int i = 0; i < w * h; i++) {
    uint16_t raw = buf[i];
    uint16_t pixel = (raw >> 8) | (raw << 8);
    int32_t r = (pixel >> 11) << 3;
    int32_t g = ((pixel >> 5) & 0x3F) << 2;
    int32_t b = (pixel & 0x1F) << 3;
    int32_t gray = (r * 306 + g * 601 + b * 117) >> 10;

    uint8_t adjusted = lut[gray];
    uint16_t q = adjusted;
    uint16_t out = ((q >> 3) << 11) | ((q >> 2) << 5) | (q >> 3);
    buf[i] = (out >> 8) | (out << 8);
    if ((i & 0xFFFF) == 0) vTaskDelay(1);
  }
}

// Reduce to N evenly spaced gray levels (8 or 4). 16 (and anything
// unexpected) is a no-op: those grays reach the 16-level panel as-is.
void applyGrayLevels(LGFX_Sprite &sprite, int levels) {
  if (levels != 8 && levels != 4) return;

  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf) return;

  const int32_t steps = levels - 1;
  for (int i = 0; i < w * h; i++) {
    uint16_t raw = buf[i];
    uint16_t pixel = (raw >> 8) | (raw << 8);
    int32_t r = (pixel >> 11) << 3;
    int32_t g = ((pixel >> 5) & 0x3F) << 2;
    int32_t b = (pixel & 0x1F) << 3;
    int32_t gray = (r * 306 + g * 601 + b * 117) >> 10;

    int32_t level = (gray * steps + 127) / 255;
    int32_t q = level * 255 / steps;
    uint16_t out = ((q >> 3) << 11) | ((q >> 2) << 5) | (q >> 3);
    buf[i] = (out >> 8) | (out << 8);
    if ((i & 0xFFFF) == 0) vTaskDelay(1);
  }
}

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

void drawZoomed(bool qualityMode) {
  static LGFX_Sprite zoomSprite(&M5.Display);
  prepareSprite(zoomSprite, DISPLAY_W, DISPLAY_H, 16, true);
  if (!zoomSprite.getBuffer() || !gSprite.getBuffer()) return;

  gSprite.setPivot(zoomCX, zoomCY);
  if (qualityMode)
    gSprite.pushRotateZoomWithAA(&zoomSprite, DISPLAY_W / 2, DISPLAY_H / 2, 0,
                                 zoomFactor, zoomFactor);
  else
    gSprite.pushRotateZoom(&zoomSprite, DISPLAY_W / 2, DISPLAY_H / 2, 0,
                           zoomFactor, zoomFactor);

  M5.Display.startWrite();
  M5.Display.setEpdMode(qualityMode ? epd_mode_t::epd_quality
                                    : epd_mode_t::epd_fastest);
  zoomSprite.pushSprite(0, 0);
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
  static LGFX_Sprite thumbSprite(&M5.Display);
  prepareSprite(thumbSprite, THUMB_IMG_W, THUMB_IMG_H, 8, true);
  if (!thumbSprite.getBuffer()) return false;
  PageData pg = loadPageData(std::string(MANGA_ROOT) + "/" + entry, 0);
  if (!pg.buf) return false;
  thumbSprite.fillScreen(TFT_WHITE);
  bool ok;
  if (isPng(pg.buf, pg.size))
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
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long sz = ftell(f);
  if (sz <= 0 || sz > 16 * 1024 * 1024) {
    fclose(f);
    return 0;
  }
  rewind(f);
  uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
  if (!buf) {
    fclose(f);
    return 0;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    heap_caps_free(buf);
    return 0;
  }
  *out = buf;
  return (size_t)sz;
}

void preloadPage(int page) {
  if (!ensurePreloader()) return;  // worker unavailable: skip preloading
  if (page < 0 || page >= totalPages) return;

  xSemaphoreTake(s_preMutex, portMAX_DELAY);
  const bool sameReady = isNextPageReady && preloadedPage == page &&
                         preloadedMangaPath == currentMangaPath;
  const bool inFlight =
      (s_req.path == currentMangaPath && s_req.page == page);
  if (!sameReady && !inFlight) {
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
  if (req.page < 0) return;

  prepareSprite(nextPageSprite, DISPLAY_W, DISPLAY_H, 16, true);
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

  bool ok = (buf != nullptr && size > 0) &&
            decodeImageToSprite(nextPageSprite, s_jpegWorker, buf, size, 0,
                                0, DISPLAY_W, DISPLAY_H);
  if (buf) heap_caps_free(buf);
  if (!ok) return;

  applyContrast(nextPageSprite, req.contrast);
  applyGrayLevels(nextPageSprite, req.gray);

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
    prepareSprite(menuCacheSprite, DISPLAY_W, DISPLAY_H, 8, true);
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
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
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
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    if (gSprite.getBuffer()) {
      size_t sz = loadFileToJpgBuffer(path.c_str());
      if (sz > 0 &&
          decodeImageToSprite(gSprite, s_jpegMain, jpgSharedBuffer(), sz, 0,
                              0, DISPLAY_W, DISPLAY_H))
        haveImage = true;
    }
  }
  if (!haveImage) {
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
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
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
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
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
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
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
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

void drawPage() {
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

    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    nextPageSprite.pushSprite(&gSprite, 0, 0);

    xSemaphoreTake(s_preMutex, portMAX_DELAY);
    isNextPageReady = false;
    xSemaphoreGive(s_preMutex);
  } else {
    ESP_LOGI(TAG, "Drawing [%d/%d] page %d", currentPage + 1, totalPages,
             currentPage);

    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    if (!gSprite.getBuffer()) {
      setCpuFrequencyMhz(80);
      drawError("Sprite alloc failed.");
      return;
    }

    PageData pg = loadPageData(currentMangaPath, currentPage);
    if (!pg.buf) {
      setCpuFrequencyMhz(80);
      drawError("Cannot open image.");
      return;
    }
    bool decodeSuccess = decodeImageToSprite(gSprite, s_jpegMain, pg.buf,
                                             pg.size, 0, 0, DISPLAY_W,
                                             DISPLAY_H);
    freePageData(pg);

    if (!decodeSuccess) {
      setCpuFrequencyMhz(80);
      drawError("Cannot decode image.");
      return;
    }
    applyContrast(gSprite, contrastPreset);
    applyGrayLevels(gSprite, grayLevels);
  }

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();

  if (currentPage < totalPages - 1) {
    preloadPage(currentPage + 1);
  }

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

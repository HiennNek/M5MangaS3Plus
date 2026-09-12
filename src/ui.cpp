// A friendly reminder from a real human:
// If VSCode's code formatter give out compilation error
// Just place SD.h before M5Unified.h
// It took me 10min to figure this out
// Fuck arduino framework
// (This is not AI generated)

#include "ui.h"
#include "bookmarks.h"
#include "icon.h"
#include "state.h"
#include "storage.h"
#include "wifi_server.h"
#include <SD.h>
#include <M5Unified.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <JPEGDEC.h>

static JPEGDEC jpeg;

struct JpegDrawContext {
    LGFX_Sprite* spr;
    int offsetX;
    int offsetY;
    int maxWidth;
    int maxHeight;
};

static int drawMCU(JPEGDRAW *pDraw) {
    JpegDrawContext* ctx = (JpegDrawContext*)pDraw->pUser;
    LGFX_Sprite* spr = ctx->spr;
    uint16_t* pixels = (uint16_t*)pDraw->pPixels;
    uint16_t* sprBuffer = (uint16_t*)spr->getBuffer();
    
    int sprWidth = spr->width();
    int sprHeight = spr->height();
    
    int cx = pDraw->x;
    int cy = pDraw->y;
    int cw = pDraw->iWidth;
    int ch = pDraw->iHeight;
    
    if (ctx->maxWidth > 0 && cx + cw > ctx->maxWidth) cw = ctx->maxWidth - cx;
    if (ctx->maxHeight > 0 && cy + ch > ctx->maxHeight) ch = ctx->maxHeight - cy;
    if (cw <= 0 || ch <= 0) return 1;

    int outX = cx + ctx->offsetX;
    int outY = cy + ctx->offsetY;

    if (outX >= sprWidth || outY >= sprHeight) return 1;
    if (outX < 0) { cw += outX; outX = 0; }
    if (outY < 0) { ch += outY; outY = 0; }
    if (outX + cw > sprWidth) cw = sprWidth - outX;
    if (outY + ch > sprHeight) ch = sprHeight - outY;

    if (cw <= 0 || ch <= 0) return 1;

    for (int y = 0; y < ch; y++) {
        memcpy(&sprBuffer[(outY + y) * sprWidth + outX], &pixels[y * pDraw->iWidth], cw * 2);
    }
    
    return 1;
}

static bool drawJpgWithJpegDec(LGFX_Sprite& spr, uint8_t* buf, size_t size, int x, int y, int maxWidth, int maxHeight) {
    JpegDrawContext ctx = {&spr, x, y, maxWidth, maxHeight};
    if (jpeg.openRAM(buf, size, drawMCU)) {
        jpeg.setPixelType(RGB565_BIG_ENDIAN);
        jpeg.setUserPointer(&ctx);
        bool res = jpeg.decode(0, 0, 0);
        jpeg.close();
        return res;
    }
    return false;
}

static bool drawJpgWithJpegDecFile(LGFX_Sprite& spr, File& file, int x, int y, int maxWidth, int maxHeight) {
    JpegDrawContext ctx = {&spr, x, y, maxWidth, maxHeight};
    if (jpeg.open(file, drawMCU)) {
        jpeg.setPixelType(RGB565_BIG_ENDIAN);
        jpeg.setUserPointer(&ctx);
        bool res = jpeg.decode(0, 0, 0);
        jpeg.close();
        return res;
    }
    return false;
}

static bool forceFullMenuRedraw = true;

static uint8_t *jpgBuffer = nullptr;
static size_t jpgBufferSize = 0;

void ensureJpgBuffer(size_t size)
{
  if (jpgBufferSize < size)
  {
    if (jpgBuffer)
      heap_caps_free(jpgBuffer);
    jpgBuffer = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (jpgBuffer)
      jpgBufferSize = size;
    else
      jpgBufferSize = 0;
  }
}

void prepareSprite(LGFX_Sprite &sprite, int w, int h, int depth,
                   bool usePsram)
{
  if (sprite.width() == w && sprite.height() == h &&
      sprite.getColorDepth() == depth)
  {
    return;
  }
  sprite.deleteSprite();
  sprite.setPsram(usePsram);
  sprite.setColorDepth(depth);
  sprite.createSprite(w, h);
}

void applyFloydSteinberg(LGFX_Sprite &sprite)
{
  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf)
    return;

  // Error buffers for current and next row (initialized to 0)
  int16_t *error_buf =
      (int16_t *)heap_caps_calloc(w * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL);
  if (!error_buf)
    return;

  int16_t *curr_err = error_buf;
  int16_t *next_err = error_buf + w;

  for (int y = 0; y < h; y++)
  {
    for (int x = 0; x < w; x++)
    {
      // LGFX RGB565 is Big-Endian on ESP32
      uint16_t raw_pixel = buf[y * w + x];
      uint16_t pixel = (raw_pixel >> 8) | (raw_pixel << 8);

      // Extract RGB565 components
      int32_t r = (pixel >> 11) << 3;
      int32_t g = ((pixel >> 5) & 0x3F) << 2;
      int32_t b = (pixel & 0x1F) << 3;

      // Simple grayscale conversion (accurate enough for manga)
      int32_t gray = (r * 306 + g * 601 + b * 117) >> 10;

      // Add error from previous pixels
      int32_t gray_with_err = gray + curr_err[x];
      if (gray_with_err < 0)
        gray_with_err = 0;
      else if (gray_with_err > 255)
        gray_with_err = 255;

      // Quantize to 16 levels (0, 17, 34 ... 255)
      // Each level is exactly 17 apart: floor((v / 17) + 0.5) * 17
      int32_t quantized = ((gray_with_err + 8) / 17) * 17;
      if (quantized > 255)
        quantized = 255;

      int32_t err = gray_with_err - quantized;

      // Pack quantized gray back to RGB565 (Big-Endian)
      uint16_t q = (uint16_t)quantized;
      uint16_t out_pixel = ((q >> 3) << 11) | ((q >> 2) << 5) | (q >> 3);
      buf[y * w + x] = (out_pixel >> 8) | (out_pixel << 8);

      // Floyd-Steinberg error distribution
      //   X   7/16
      // 3/16 5/16 1/16
      if (x + 1 < w)
        curr_err[x + 1] += (err * 7) >> 4;
      if (y + 1 < h)
      {
        if (x > 0)
          next_err[x - 1] += (err * 3) >> 4;
        next_err[x] += (err * 5) >> 4;
        if (x + 1 < w)
          next_err[x + 1] += (err * 1) >> 4;
      }
    }
    // Swap error buffers for next row
    int16_t *tmp = curr_err;
    curr_err = next_err;
    next_err = tmp;
    memset(next_err, 0, w * sizeof(int16_t));
  }

  heap_caps_free(error_buf);
}

// Atkinson dithering — distributes only 6/8 of the error (not full),
// producing higher contrast and sharper edges. Ideal for manga lineart.
void applyAtkinson(LGFX_Sprite &sprite)
{
  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf)
    return;

  // Atkinson looks ahead 2 columns and 2 rows, so we need 3 row buffers
  int16_t *error_buf =
      (int16_t *)heap_caps_calloc(w * 3, sizeof(int16_t), MALLOC_CAP_INTERNAL);
  if (!error_buf)
    return;

  int16_t *row0 = error_buf;         // current row
  int16_t *row1 = error_buf + w;     // next row
  int16_t *row2 = error_buf + w * 2; // row after next

  for (int y = 0; y < h; y++)
  {
    for (int x = 0; x < w; x++)
    {
      uint16_t raw_pixel = buf[y * w + x];
      uint16_t pixel = (raw_pixel >> 8) | (raw_pixel << 8);
      int32_t r = (pixel >> 11) << 3;
      int32_t g = ((pixel >> 5) & 0x3F) << 2;
      int32_t b = (pixel & 0x1F) << 3;
      int32_t gray = (r * 306 + g * 601 + b * 117) >> 10;

      int32_t gray_with_err = gray + row0[x];
      if (gray_with_err < 0)
        gray_with_err = 0;
      else if (gray_with_err > 255)
        gray_with_err = 255;

      int32_t quantized = ((gray_with_err + 8) / 17) * 17;
      if (quantized > 255)
        quantized = 255;

      // Atkinson distributes 6/8 of error (loses 2/8 → higher contrast)
      int32_t err = (gray_with_err - quantized) >> 3; // err/8 unit

      uint16_t q = (uint16_t)quantized;
      uint16_t out_pixel = ((q >> 3) << 11) | ((q >> 2) << 5) | (q >> 3);
      buf[y * w + x] = (out_pixel >> 8) | (out_pixel << 8);

      // Atkinson error distribution pattern:
      //       X   1/8  1/8
      //  1/8  1/8  1/8
      //       1/8
      if (x + 1 < w)
        row0[x + 1] += err;
      if (x + 2 < w)
        row0[x + 2] += err;
      if (y + 1 < h)
      {
        if (x > 0)
          row1[x - 1] += err;
        row1[x] += err;
        if (x + 1 < w)
          row1[x + 1] += err;
      }
      if (y + 2 < h)
      {
        row2[x] += err;
      }
    }
    // Rotate row buffers
    int16_t *tmp = row0;
    row0 = row1;
    row1 = row2;
    row2 = tmp;
    memset(row2, 0, w * sizeof(int16_t));
  }

  heap_caps_free(error_buf);
}

// Ordered (Bayer 4x4) dithering — fastest, no error propagation.
// Produces a regular pattern. Good for fast previews.
static const int8_t bayer4x4[4][4] = {
    {-8, 0, -6, 2}, {4, -4, 6, -2}, {-5, 3, -7, 1}, {7, -1, 5, -3}};

void applyOrderedBayer(LGFX_Sprite &sprite)
{
  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf)
    return;

  for (int y = 0; y < h; y++)
  {
    for (int x = 0; x < w; x++)
    {
      uint16_t raw_pixel = buf[y * w + x];
      uint16_t pixel = (raw_pixel >> 8) | (raw_pixel << 8);
      int32_t r = (pixel >> 11) << 3;
      int32_t g = ((pixel >> 5) & 0x3F) << 2;
      int32_t b = (pixel & 0x1F) << 3;
      int32_t gray = (r * 306 + g * 601 + b * 117) >> 10;

      // Add Bayer threshold bias (scaled to match 16-level quantization)
      gray += bayer4x4[y & 3][x & 3];
      if (gray < 0)
        gray = 0;
      else if (gray > 255)
        gray = 255;

      int32_t quantized = ((gray + 8) / 17) * 17;
      if (quantized > 255)
        quantized = 255;

      uint16_t q = (uint16_t)quantized;
      uint16_t out_pixel = ((q >> 3) << 11) | ((q >> 2) << 5) | (q >> 3);
      buf[y * w + x] = (out_pixel >> 8) | (out_pixel << 8);
    }
  }
}

// Dispatcher — applies the selected dithering algorithm
void applyDithering(LGFX_Sprite &sprite)
{
  switch (ditherMode)
  {
  case DITHER_FLOYD_STEINBERG:
    applyFloydSteinberg(sprite);
    break;
  case DITHER_ATKINSON:
    applyAtkinson(sprite);
    break;
  case DITHER_ORDERED:
    applyOrderedBayer(sprite);
    break;
  default:
    break; // DITHER_OFF — do nothing
  }
}

// Human-readable label for the current dither mode
const char *ditherModeName()
{
  switch (ditherMode)
  {
  case DITHER_OFF:
    return "OFF";
  case DITHER_FLOYD_STEINBERG:
    return "FLOYD-STEINBERG";
  case DITHER_ATKINSON:
    return "ATKINSON";
  case DITHER_ORDERED:
    return "BAYER 4x4";
  default:
    return "UNKNOWN";
  }
}

// Apply brightness/contrast adjustment using a precomputed LUT.
// This runs on the RGB565 sprite in-place, before dithering.
void applyContrast(LGFX_Sprite &sprite)
{
  if (contrastPreset == CONTRAST_NORMAL)
    return; // No-op

  int w = sprite.width();
  int h = sprite.height();
  uint16_t *buf = (uint16_t *)sprite.getBuffer();
  if (!buf)
    return;

  // Precompute a 256-entry grayscale LUT for the selected preset.
  // Using fixed-point: contrast is 8.8 fixed (256 = 1.0x)
  int32_t contrast_fp, brightness;
  switch (contrastPreset)
  {
  case CONTRAST_VIVID:
    contrast_fp = 307;
    brightness = 0;
    break; // 1.2x
  case CONTRAST_HIGH:
    contrast_fp = 358;
    brightness = 5;
    break; // 1.4x
  case CONTRAST_LIGHT:
    contrast_fp = 256;
    brightness = 30;
    break; // 1.0x + bright
  default:
    return;
  }

  uint8_t lut[256];
  for (int i = 0; i < 256; i++)
  {
    int32_t val = (((i - 128) * contrast_fp) >> 8) + 128 + brightness;
    if (val < 0)
      val = 0;
    else if (val > 255)
      val = 255;
    lut[i] = (uint8_t)val;
  }

  // Apply LUT to every pixel (convert to gray, adjust, write back)
  for (int i = 0; i < w * h; i++)
  {
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
  }
}

const char *contrastPresetName()
{
  switch (contrastPreset)
  {
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

// Standardized modern button helper
void drawModernButton(LGFX_Sprite &sprite, int x, int y, int w, int h,
                      const char *text, bool isPrimary)
{
  if (isPrimary)
  {
    sprite.fillRoundRect(x, y, w, h, UI_RADIUS, UI_FG);
    sprite.setTextColor(UI_BG, UI_FG);
  }
  else
  {
    sprite.fillRoundRect(x, y, w, h, UI_RADIUS, UI_BG);
    sprite.drawRoundRect(x, y, w, h, UI_RADIUS, UI_BORDER);
    sprite.setTextColor(UI_FG, UI_BG);
  }
  sprite.setFont(&fonts::DejaVu18);
  sprite.setTextDatum(middle_center);
  sprite.drawString(text, x + w / 2, y + h / 2);
  sprite.setTextDatum(top_left);
}

static int lastOutX = -1, lastOutY = -1;

void drawMagnifier(int x, int y, bool qualityMode)
{
  static LGFX_Sprite magSprite(&M5.Display);
  prepareSprite(magSprite, MAG_SIZE, MAG_SIZE, 16, true);
  if (!magSprite.getBuffer())
    return;

  // 1. Calculate new position
  int outX = x - MAG_SIZE / 2;
  int outY = y - MAG_SIZE - 60;
  if (outX < 10)
    outX = 10;
  if (outX + MAG_SIZE > DISPLAY_W - 10)
    outX = DISPLAY_W - MAG_SIZE - 10;
  if (outY < 10)
    outY = 10;

  // 2. Prepare the zoomed image
  int srcSize = MAG_SIZE / MAG_SCALE;
  int srcX = x, srcY = y;
  if (srcX < srcSize / 2)
    srcX = srcSize / 2;
  if (srcY < srcSize / 2)
    srcY = srcSize / 2;
  if (srcX > DISPLAY_W - srcSize / 2)
    srcX = DISPLAY_W - srcSize / 2;
  if (srcY > DISPLAY_H - srcSize / 2)
    srcY = DISPLAY_H - srcSize / 2;

  magSprite.fillScreen(TFT_WHITE);
  gSprite.setPivot(srcX, srcY);
  gSprite.pushRotateZoom(&magSprite, MAG_SIZE / 2, MAG_SIZE / 2, 0, MAG_SCALE,
                         MAG_SCALE);

  magSprite.drawRect(0, 0, MAG_SIZE, MAG_SIZE, TFT_BLACK);
  magSprite.drawRect(1, 1, MAG_SIZE - 2, MAG_SIZE - 2, TFT_WHITE);
  magSprite.drawRect(2, 2, MAG_SIZE - 4, MAG_SIZE - 4, TFT_BLACK);

  // 3. CLEANUP: If we moved, erase the old magnifier position using gSprite
  M5.Display.startWrite();
  if (lastOutX != -1 && (lastOutX != outX || lastOutY != outY))
  {
    // Restore background from gSprite to the OLD magnifier location
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    M5.Display.setClipRect(lastOutX, lastOutY, MAG_SIZE, MAG_SIZE);
    gSprite.pushSprite(0, 0);
    M5.Display.clearClipRect();
  }

  // 4. DRAW: Draw the new magnifier
  M5.Display.setEpdMode(qualityMode ? epd_mode_t::epd_quality
                                    : epd_mode_t::epd_fast);
  magSprite.pushSprite(outX, outY);

  M5.Display.display();
  M5.Display.endWrite();

  lastOutX = outX;
  lastOutY = outY;
}

// Reset tracking when magnifier closes
void resetMagnifierTracking()
{
  lastOutX = -1;
  lastOutY = -1;
}

void preloadPage(int page)
{
  if (page < 0 || page >= totalPages)
  {
    isNextPageReady = false;
    return;
  }
  if (isNextPageReady && preloadedPage == page &&
      preloadedMangaPath == currentMangaPath)
  {
    return;
  }

  setCpuFrequencyMhz(240);
  String path = makePagePath(currentMangaPath, page);

  prepareSprite(nextPageSprite, DISPLAY_W, DISPLAY_H, 16, true);
  if (!nextPageSprite.getBuffer())
  {
    isNextPageReady = false;
    return;
  }

  File pageFile = SD.open(path.c_str());
  if (!pageFile)
  {
    isNextPageReady = false;
    return;
  }

  size_t fileSize = pageFile.size();
  ensureJpgBuffer(fileSize + 1024);

  bool success = false;
  if (jpgBuffer)
  {
    pageFile.read(jpgBuffer, fileSize);
    pageFile.close();
    success = drawJpgWithJpegDec(nextPageSprite, jpgBuffer, fileSize, 0, 0, DISPLAY_W, DISPLAY_H);
  }
  else
  {
    success = drawJpgWithJpegDecFile(nextPageSprite, pageFile, 0, 0, DISPLAY_W, DISPLAY_H);
    pageFile.close();
  }

  if (success)
  {
    applyContrast(nextPageSprite);
    if (ditherMode != DITHER_OFF)
    {
      applyDithering(nextPageSprite);
    }
    preloadedPage = page;
    preloadedMangaPath = currentMangaPath;
    isNextPageReady = true;
  }
  else
  {
    isNextPageReady = false;
  }
  setCpuFrequencyMhz(80);
}

void drawMenu()
{
  static String drawnLastMangaName = "";
  static int drawnLastPage = -1;
  static int drawnLastMenuScroll = -1;

  int totalItems = (int)mangaFolders.size() + 2;
  int end = std::min(totalItems, menuScroll + MENU_VISIBLE);

  if (!menuCacheValid || lastDrawnMenuScroll != menuScroll)
  {
    prepareSprite(menuCacheSprite, DISPLAY_W, DISPLAY_H, 8, true);
    if (menuCacheSprite.getBuffer())
    {
      menuCacheSprite.fillScreen(UI_BG);
      menuCacheSprite.drawLine(0, 80, DISPLAY_W, 80, UI_BORDER);
      menuCacheSprite.setFont(&fonts::DejaVu24);
      menuCacheSprite.setTextColor(UI_FG, UI_BG);
      menuCacheSprite.setCursor(GRID_GUTTER, 30);
      menuCacheSprite.print("Library");

      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setTextColor(UI_FG, UI_BG);
      menuCacheSprite.setCursor(GRID_GUTTER, 60);
      menuCacheSprite.printf("%d titles available", (int)mangaFolders.size());

      if (totalItems > 0)
      {
        int curPg = (menuScroll / MENU_VISIBLE) + 1;
        int maxPg = (totalItems + MENU_VISIBLE - 1) / MENU_VISIBLE;
        menuCacheSprite.setTextColor(UI_FG, UI_BG);
        menuCacheSprite.setCursor(DISPLAY_W - 100, 30);
        menuCacheSprite.printf("Pg %d/%d", curPg, maxPg);
      }

      if (mangaFolders.empty())
      {
        menuCacheSprite.setFont(&fonts::DejaVu18);
        menuCacheSprite.setTextColor(TFT_BLACK, TFT_WHITE);
        menuCacheSprite.setCursor(GRID_GUTTER, 120);
        menuCacheSprite.println("No manga found in /manga/");
      }
      else
      {
        for (int i = menuScroll; i < end; i++)
        {
          int relIdx = i - menuScroll;
          int row = relIdx / GRID_COLS;
          int col = relIdx % GRID_COLS;
          int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
          int y = GRID_Y_TOP + row * GRID_ROW_H;

          menuCacheSprite.fillRoundRect(x, y, THUMB_W, THUMB_H, UI_RADIUS,
                                        UI_BG);
          menuCacheSprite.drawRoundRect(x, y, THUMB_W, THUMB_H, UI_RADIUS,
                                        UI_BORDER);

          if (i == 0)
          {
            menuCacheSprite.fillRoundRect(x + 10, y + 10, THUMB_W - 20,
                                          THUMB_H - 20, UI_RADIUS, UI_ACCENT);
            int iconX = x + (THUMB_W - 96) / 2;
            int iconY = y + (THUMB_H - 96) / 2;
            menuCacheSprite.drawBitmap(
                iconX, iconY, image_book_70dp_1F1F1F_bits, 96, 96, UI_FG);
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString("BOOKMARKS", x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          }
          else if (i == 1)
          {
            menuCacheSprite.fillRoundRect(x + 10, y + 10, THUMB_W - 20,
                                          THUMB_H - 20, UI_RADIUS, UI_ACCENT);
            int iconX = x + (THUMB_W - 96) / 2;
            int iconY = y + (THUMB_H - 96) / 2;
            menuCacheSprite.drawBitmap(
                iconX, iconY, image_drive_folder_upload_70dp_1F1F1F_bits, 96,
                96, UI_FG);
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString("FILES", x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          }
          else
          {
            int fIdx = i - 2;
            String coverPath =
                makePagePath(String(MANGA_ROOT) + "/" + mangaFolders[fIdx], 0);
            File coverFile = SD.open(coverPath.c_str());
            if (coverFile)
            {
              float imgAspect = 540.0f / 960.0f;
              int scaledW = (int)((THUMB_H - 4) * imgAspect);
              int xOffset = (THUMB_W - 4 - scaledW) / 2;
              if (xOffset < 0)
                xOffset = 0;
              menuCacheSprite.drawJpg(&coverFile, x + 2 + xOffset, y + 2, THUMB_W - 4, THUMB_H - 4, 0, 0, 0.0f, 0.0f);
              coverFile.close();
            }
            else
            {
              menuCacheSprite.setTextColor(UI_FG, UI_BG);
              menuCacheSprite.setFont(&fonts::DejaVu12);
              menuCacheSprite.setCursor(x + 10, y + THUMB_H / 2);
              menuCacheSprite.print("NO COVER");
            }
            menuCacheSprite.setFont(&fonts::DejaVu12);
            menuCacheSprite.setTextColor(UI_FG, UI_BG);
            String title = mangaFolders[fIdx];
            if (title.length() > 22)
              title = title.substring(0, 20) + "...";

            menuCacheSprite.setTextDatum(top_center);
            menuCacheSprite.drawString(title, x + THUMB_W / 2,
                                       y + THUMB_H + 10);
            menuCacheSprite.setTextDatum(top_left);
          }
        }
      }
      menuCacheValid = true;
      drawnLastPage = -1; // Force resume bar redraw
      lastDrawnMenuScroll = menuScroll;
      forceFullMenuRedraw = true;
    }
  }

  if (menuCacheValid &&
      (drawnLastMangaName != lastMangaName || drawnLastPage != lastPage ||
       drawnLastMenuScroll != menuScroll))
  {
    int barW = DISPLAY_W - 40;
    int barH = 60;
    int barX = 20;
    int barY = DISPLAY_H - 85;

    if (lastMangaName.length() == 0)
    {
      menuCacheSprite.fillRect(barX, barY - 2, barW + 4, barH + 4,
                               UI_BG); // Erase shadow + bar
    }
    else
    {
      // Shadow
      menuCacheSprite.fillRoundRect(barX + 2, barY + 2, barW, barH, UI_RADIUS,
                                    UI_SHADOW);
      // Pill
      menuCacheSprite.fillRoundRect(barX, barY, barW, barH, UI_RADIUS, UI_BG);
      menuCacheSprite.drawRoundRect(barX, barY, barW, barH, UI_RADIUS,
                                    UI_BORDER);

      menuCacheSprite.setTextColor(UI_FG, UI_BG);
      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setCursor(barX + 15, barY + 12);
      menuCacheSprite.print("CONTINUE: ");

      menuCacheSprite.setFont(&fonts::DejaVu18);
      String shortName = lastMangaName;
      if (shortName.length() > 19)
        shortName = shortName.substring(0, 17) + "...";
      menuCacheSprite.print(shortName);

      menuCacheSprite.setFont(&fonts::DejaVu12);
      menuCacheSprite.setCursor(barX + 15, barY + 36);
      menuCacheSprite.printf("Page %d", lastPage + 1);

      // "RESUME" Button
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
  if (lastMenuScrollForDirty != menuScroll)
    isFullRedraw = true;
  lastMenuScrollForDirty = menuScroll;
  forceFullMenuRedraw = false;

  M5.Display.startWrite();
  if (isFullRedraw)
  {
    if (menuCacheValid)
      menuCacheSprite.pushSprite(0, 0);
    else
      M5.Display.fillScreen(TFT_WHITE);
    for (int i = menuScroll; i < end; i++)
    {
      if (i == menuSelected)
      {
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
  }
  else
  {
    if (prevMenuSelected >= menuScroll && prevMenuSelected < end)
    {
      int relIdx = prevMenuSelected - menuScroll;
      int row = relIdx / GRID_COLS;
      int col = relIdx % GRID_COLS;
      int x = GRID_GUTTER + col * (THUMB_W + GRID_GUTTER);
      int y = GRID_Y_TOP + row * GRID_ROW_H;
      M5.Display.setClipRect(x - 6, y - 6, THUMB_W + 12, THUMB_H + 40);
      menuCacheSprite.pushSprite(0, 0);
      M5.Display.clearClipRect();
    }
    if (menuSelected >= menuScroll && menuSelected < end)
    {
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

void drawControlCenter()
{
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
  if (!gSprite.getBuffer())
    return;
  gSprite.fillScreen(TFT_MAGENTA); // Use magenta as transparent color
  int modW = 500;
  int modH = 300;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  // Drop shadow
  gSprite.fillRoundRect(modX + 6, modY + 6, modW, modH, UI_RADIUS, UI_SHADOW);

  // Modal
  gSprite.fillRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BG);
  gSprite.drawRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BORDER);
  gSprite.drawRoundRect(modX + 1, modY + 1, modW - 2, modH - 2, UI_RADIUS - 1,
                        UI_BORDER);
  gSprite.drawRoundRect(modX + 2, modY + 2, modW - 4, modH - 4, UI_RADIUS - 2,
                        UI_BORDER);

  // Header
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
  gSprite.fillRoundRect(pX + 4, pY + 4, (pW - 8) * bat / 100, 32, UI_RADIUS - 2,
                        UI_FG);

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

void systemShutdown()
{
  setCpuFrequencyMhz(240);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  std::vector<String> pics;
  File root = SD.open(PIC_ROOT);
  if (root && root.isDirectory())
  {
    File entry;
    while ((entry = root.openNextFile()))
    {
      if (!entry.isDirectory())
      {
        String name = entry.name();
        if (name.endsWith(".jpg") || name.endsWith(".jpeg") ||
            name.endsWith(".JPG") || name.endsWith(".JPEG"))
        {
          pics.push_back(name);
        }
      }
      entry.close();
    }
    root.close();
  }
  bool drawn = false;
  if (!pics.empty())
  {
    int r = random(pics.size());
    String path = pics[r];
    if (!path.startsWith("/"))
      path = String(PIC_ROOT) + "/" + path;
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    if (gSprite.getBuffer())
    {
      File f = SD.open(path.c_str());
      if (f)
      {
        if (drawJpgWithJpegDecFile(gSprite, f, 0, 0, DISPLAY_W, DISPLAY_H))
        {
          M5.Display.startWrite();
          gSprite.pushSprite(0, 0);
          M5.Display.display();
          M5.Display.endWrite();
          drawn = true;
        }
        f.close();
      }
    }
  }
  if (!drawn)
  {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.display();
  }
  delay(2000);
  M5.Power.powerOff();
}

void drawBookConfig()
{
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
  if (!gSprite.getBuffer())
    return;
  gSprite.fillScreen(TFT_MAGENTA); // Use magenta as transparent color
  int modW = 460;
  int modH = 490;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  // Drop shadow
  gSprite.fillRoundRect(modX + 6, modY + 6, modW, modH, UI_RADIUS, UI_SHADOW);

  // Modal
  gSprite.fillRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BG);
  gSprite.drawRoundRect(modX, modY, modW, modH, UI_RADIUS, UI_BORDER);
  gSprite.drawRoundRect(modX + 1, modY + 1, modW - 2, modH - 2, UI_RADIUS - 1,
                        UI_BORDER);
  gSprite.drawRoundRect(modX + 2, modY + 2, modW - 4, modH - 4, UI_RADIUS - 2,
                        UI_BORDER);

  // Header
  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setTextDatum(top_center);
  gSprite.drawString("Book Menu", modX + modW / 2, modY + 25);
  gSprite.setTextDatum(top_left);
  gSprite.drawLine(modX, modY + 70, modX + modW, modY + 70, UI_BORDER);

  int barY = modY + 85;
  gSprite.drawRoundRect(modX + 20, barY, modW - 40, 80, UI_RADIUS, UI_BORDER);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(modX + 45, barY + 25);
  gSprite.print("<<");
  gSprite.setCursor(modX + 115, barY + 25);
  gSprite.print("<");
  String pg = String(bookConfigPendingPage + 1) + " / " + String(totalPages);
  int pgW = pg.length() * 14;
  gSprite.setCursor(modX + (modW - pgW) / 2, barY + 25);
  gSprite.print(pg);
  gSprite.setCursor(modX + modW - 135, barY + 25);
  gSprite.print(">");
  gSprite.setCursor(modX + modW - 85, barY + 25);
  gSprite.print(">>");

  gSprite.drawLine(modX, modY + 180, modX + modW, modY + 180, UI_BORDER);

  int btnW = modW - 60;
  int btnX = modX + 30;
  int btnH = 60;

  int btnY0 = modY + 190;
  String ditherMsg = String("DITHER: ") + ditherModeName();
  drawModernButton(gSprite, btnX, btnY0, btnW, btnH, ditherMsg.c_str(), false);

  int btnY1 = modY + 260;
  String contrastMsg = String("CONTRAST: ") + contrastPresetName();
  drawModernButton(gSprite, btnX, btnY1, btnW, btnH, contrastMsg.c_str(),
                   false);

  int btnY2 = modY + 330;
  drawModernButton(gSprite, btnX, btnY2, btnW, btnH, "BOOKMARK PAGE", false);

  int btnY3 = modY + 400;
  drawModernButton(gSprite, btnX, btnY3, btnW, btnH, "RETURN TO LIBRARY", true);

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0, TFT_MAGENTA);
  M5.Display.display();
  M5.Display.endWrite();
}

void drawBookmarks()
{
  forceFullMenuRedraw = true;
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
  if (!gSprite.getBuffer())
    return;
  gSprite.fillScreen(UI_BG);
  gSprite.drawLine(0, 80, DISPLAY_W, 80, UI_BORDER);
  gSprite.setTextColor(UI_FG, UI_BG);
  gSprite.setFont(&fonts::DejaVu24);
  gSprite.setCursor(20, 30);
  if (selectedBookmarkFolder == "")
    gSprite.print("Bookmark Library");
  else
  {
    String t = selectedBookmarkFolder;
    if (t.length() > 19)
      t = t.substring(0, 17) + "...";
    gSprite.printf("< %s", t.c_str());
  }
  int itemsPerPage = (DISPLAY_H - 200) / 90;
  int totalItems = 0;
  if (bookmarks.empty())
  {
    gSprite.setTextColor(UI_FG, UI_BG);
    gSprite.setFont(&fonts::DejaVu18);
    gSprite.setCursor(40, 150);
    gSprite.print("No bookmarks yet.");
  }
  else
  {
    int yOff = 100;
    int itemH = 80;
    if (selectedBookmarkFolder == "")
    {
      auto uniqueFolders = getUniqueBookmarkFolders();
      totalItems = uniqueFolders.size();
      int count = 0;
      for (int i = 0; i < (int)uniqueFolders.size(); i++)
      {
        if (i < bookmarkScroll)
          continue;
        if (count >= itemsPerPage)
          break;
        gSprite.fillRoundRect(10 + 4, yOff + 4, DISPLAY_W - 20, itemH,
                              UI_RADIUS, UI_SHADOW);
        gSprite.fillRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BG);
        gSprite.drawRoundRect(10, yOff, DISPLAY_W - 20, itemH, UI_RADIUS,
                              UI_BORDER);
        gSprite.setTextColor(UI_FG, UI_BG);
        gSprite.setFont(&fonts::DejaVu18);
        gSprite.setCursor(30, yOff + 30);
        String t = uniqueFolders[i];
        if (t.length() > 26)
          t = t.substring(0, 24) + "...";
        gSprite.print(t);
        gSprite.setFont(&fonts::DejaVu12);
        gSprite.setTextColor(UI_FG, UI_BG);
        gSprite.setCursor(DISPLAY_W - 80, yOff + 34);
        gSprite.print("OPEN >");
        yOff += itemH + 10;
        count++;
      }
    }
    else
    {
      int count = 0;
      int folderItemIdx = 0;
      for (int i = 0; i < (int)bookmarks.size(); i++)
      {
        if (bookmarks[i].folder != selectedBookmarkFolder)
          continue;
        totalItems++;
        if (folderItemIdx < bookmarkScroll)
        {
          folderItemIdx++;
          continue;
        }
        if (count >= itemsPerPage)
        {
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
  if (totalItems > 0)
  {
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

void drawWifiServer()
{
  forceFullMenuRedraw = true;
  if (!isWifiServerRunning())
    startWifiServer();
  prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 8, true);
  if (!gSprite.getBuffer())
    return;
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
  gSprite.print(getWifiSSID());
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

void drawPage()
{
  forceFullMenuRedraw = true;
  if (totalPages == 0)
  {
    drawError("No images in this manga.");
    return;
  }

  setCpuFrequencyMhz(240);

  // 1. Instant turn logic: Check if requested page is preloaded
  if (isNextPageReady && preloadedPage == currentPage &&
      preloadedMangaPath == currentMangaPath)
  {
    Serial.printf("Instant turn for page %d\n", currentPage + 1);

    // Swap buffers/sprites or just draw nextPageSprite to gSprite
    // The most robust way in LGFX without direct pointer hacking is pushSprite
    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    nextPageSprite.pushSprite(&gSprite, 0, 0);

    isNextPageReady = false; // Used it up
  }
  else
  {
    String path = makePagePath(currentMangaPath, currentPage);
    Serial.printf("Drawing [%d/%d]: %s\n", currentPage + 1, totalPages,
                  path.c_str());

    prepareSprite(gSprite, DISPLAY_W, DISPLAY_H, 16, true);
    if (!gSprite.getBuffer())
    {
      setCpuFrequencyMhz(80);
      drawError("Sprite alloc failed.");
      return;
    }

    File pageFile = SD.open(path.c_str());
    if (!pageFile)
    {
      setCpuFrequencyMhz(80);
      drawError("Cannot open image.");
      return;
    }

    size_t fileSize = pageFile.size();
    ensureJpgBuffer(fileSize + 1024);
    bool decodeSuccess = false;

    if (jpgBuffer)
    {
      pageFile.read(jpgBuffer, fileSize);
      pageFile.close();
      decodeSuccess = drawJpgWithJpegDec(gSprite, jpgBuffer, fileSize, 0, 0, DISPLAY_W, DISPLAY_H);
    }
    else
    {
      decodeSuccess = drawJpgWithJpegDecFile(gSprite, pageFile, 0, 0, DISPLAY_W, DISPLAY_H);
      pageFile.close();
    }

    if (!decodeSuccess)
    {
      setCpuFrequencyMhz(80);
      drawError("Cannot decode image.");
      return;
    }
    applyContrast(gSprite);
    if (ditherMode != DITHER_OFF)
    {
      applyDithering(gSprite);
    }
  }

  M5.Display.startWrite();
  gSprite.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.endWrite();

  // 2. Preload NEXT page in background (after rendering current)
  if (currentPage < totalPages - 1)
  {
    preloadPage(currentPage + 1);
  }

  setCpuFrequencyMhz(80);
}

void drawError(const char *msg)
{
  forceFullMenuRedraw = true;
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(20, 40);
  M5.Display.println(msg);
  M5.Display.display();
}

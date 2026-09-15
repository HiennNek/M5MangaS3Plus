#pragma once

#include <M5GFX.h>

#include "config.h"
#include "state.h"

// Book Menu modal layout, shared by drawBookConfig() (ui.cpp) and
// handleBookConfigTouch() (input.cpp). Origin = modal top-left.
#define BOOK_MOD_W 460
#define BOOK_MOD_H 545
#define BOOK_PAGE_Y 85
#define BOOK_PAGE_H 70
#define BOOK_CHAP_Y 165
#define BOOK_CHAP_H 60
#define BOOK_DIV_Y 245
#define BOOK_BTN_XOFF 30
#define BOOK_BTN_W (BOOK_MOD_W - 60)
#define BOOK_BTN_H 60
#define BOOK_BTN_DITHER_Y 255
#define BOOK_BTN_CONTRAST_Y 325
#define BOOK_BTN_BOOKMARK_Y 395
#define BOOK_BTN_RETURN_Y 465
// Chapter stepper tap zones, measured from the modal left/right edge.
#define BOOK_CHAP_ZONE 120

void drawMenu();
void drawControlCenter();
void drawBookConfig();
void drawBookmarks();
void drawWifiServer();
void drawPage();
void preloadPage(int page);
void drawZoomed(bool qualityMode);
void clampZoomViewport();
void applyFloydSteinberg(LGFX_Sprite &sprite);
void applyAtkinson(LGFX_Sprite &sprite);
void applyOrderedBayer(LGFX_Sprite &sprite);
void applyDithering(LGFX_Sprite &sprite);
const char *ditherModeName();
void applyContrast(LGFX_Sprite &sprite);
const char *contrastPresetName();
void drawError(const char *msg);
void systemShutdown();
// Full original-quality refresh of gSprite's current content (~36 scans,
// ends ghost-free). See tools/patch_epd_lut.py. Used for the power-off
// splash that persists on screen.
void fullRefresh();
void drawModernButton(LGFX_Sprite &sprite, int x, int y, int w, int h,
                      const char *text, bool isPrimary = false);

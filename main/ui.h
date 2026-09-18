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
#define BOOK_BTN_GRAY_Y 255
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
// Page sprites use PAGE_DEPTH: Panel_EPD's write depth is grayscale_8bit,
// so a grayscale sprite pushes with no colour conversion and half the PSRAM
// traffic of rgb565. Menus stay on rgb332 (PAGE_DEPTH is not a drop-in for
// them -- their raw-gray icon blits assume the existing behaviour).
#define PAGE_DEPTH lgfx::v1::color_depth_t::grayscale_8bit
#define UI_DEPTH lgfx::v1::color_depth_t::rgb332_1Byte

void prepareSprite(LGFX_Sprite &sprite, int w, int h,
                   lgfx::v1::color_depth_t depth, bool usePsram);
const char *contrastPresetName();
void drawError(const char *msg);
void systemShutdown();
// Full original-quality refresh of gSprite's current content (~36 scans,
// ends ghost-free). See tools/patch_epd_lut.py. Used for the power-off
// splash that persists on screen.
void fullRefresh();
void drawModernButton(LGFX_Sprite &sprite, int x, int y, int w, int h,
                      const char *text, bool isPrimary = false);

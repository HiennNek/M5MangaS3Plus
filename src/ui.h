#pragma once

#include "config.h"
#include "state.h"

void drawMenu();
void drawControlCenter();
void drawBookConfig();
void drawBookmarks();
void drawWifiServer();
void drawPage();
void preloadPage(int page);
void drawMagnifier(int x, int y, bool qualityMode = false);
void resetMagnifierTracking();
void applyFloydSteinberg(LGFX_Sprite &sprite);
void applyAtkinson(LGFX_Sprite &sprite);
void applyOrderedBayer(LGFX_Sprite &sprite);
void applyDithering(LGFX_Sprite &sprite);
const char *ditherModeName();
void applyContrast(LGFX_Sprite &sprite);
const char *contrastPresetName();
void drawError(const char *msg);
void systemShutdown();
void drawModernButton(LGFX_Sprite &sprite, int x, int y, int w, int h,
                      const char *text, bool isPrimary = false);

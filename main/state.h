#pragma once

#include <M5GFX.h>

#include <string>
#include <vector>

#include "config.h"

struct Bookmark {
  std::string folder;
  int page;
};

extern AppState appState;
extern std::vector<std::string> mangaFolders;
extern std::vector<int> mangaPageCounts;
extern int menuSelected;
extern int menuScroll;
extern int bookmarkScroll;

extern std::string currentMangaPath;
extern int totalPages;
extern int currentPage;
extern bool needRedraw;
extern bool controlMenuOpen;
extern bool bookConfigOpen;
extern int bookConfigPendingPage;
extern ContrastPreset contrastPreset;
extern int grayLevels;  // 16 (default), 8, or 4; contrast applies only at 16
extern bool isZoomed;
extern float zoomFactor;
extern int zoomCX, zoomCY;  // zoom viewport center, in page pixels
extern std::string selectedBookmarkFolder;
extern epd_mode_t currentEpdMode;

// Current / preloaded / previous page each live in an identical sprite.
// Pages move between roles by exchanging pointers, never by copying. The
// names stay usable as plain objects so call sites did not change.
extern LGFX_Sprite *gSpritePtr;
extern LGFX_Sprite *nextPageSpritePtr;
extern LGFX_Sprite *prevPageSpritePtr;
#define gSprite (*gSpritePtr)
#define nextPageSprite (*nextPageSpritePtr)
#define prevPageSprite (*prevPageSpritePtr)

// Preloaded page becomes current; the outgoing page is kept in the third
// slot instead of being recycled, so turning back does not decode again.
void rotatePageSprites();
// Current <-> previous, for a back-turn that hits the kept page.
void swapPrevPageSprite();
extern LGFX_Sprite menuCacheSprite;
extern int lastDrawnMenuScroll;
extern bool menuCacheValid;
extern std::string lastMangaPath;
extern int lastPage;
extern std::string lastMangaName;

extern std::string preloadedMangaPath;
extern int preloadedPage;
extern bool isNextPageReady;

extern std::vector<Bookmark> bookmarks;

inline void requestRedraw(epd_mode_t mode = epd_mode_t::epd_fast) {
  currentEpdMode = mode;
  needRedraw = true;
}

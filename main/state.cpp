#include "state.h"

#include <M5Unified.h>

#include <utility>

AppState appState = STATE_MENU;
std::vector<std::string> mangaFolders;
std::vector<int> mangaPageCounts;
int menuSelected = 0;
int menuScroll = 0;
int bookmarkScroll = 0;

std::string currentMangaPath;
int totalPages = 0;
int currentPage = 0;
bool needRedraw = true;
bool controlMenuOpen = false;
bool bookConfigOpen = false;
int bookConfigPendingPage = 0;
ContrastPreset contrastPreset = CONTRAST_NORMAL;
int grayLevels = 16;
bool isZoomed = false;
float zoomFactor = 1.0f;
int zoomCX = DISPLAY_W / 2, zoomCY = DISPLAY_H / 2;
std::string selectedBookmarkFolder = "";
epd_mode_t currentEpdMode = epd_mode_t::epd_fast;

static LGFX_Sprite s_pageA(&M5.Display);
static LGFX_Sprite s_pageB(&M5.Display);
static LGFX_Sprite s_pageC(&M5.Display);
LGFX_Sprite *gSpritePtr = &s_pageA;
LGFX_Sprite *nextPageSpritePtr = &s_pageB;
LGFX_Sprite *prevPageSpritePtr = &s_pageC;

// O(1) page handoff. All three sprites are DISPLAY_W x DISPLAY_H at the same
// depth, so pages move by exchanging pointers rather than copying through
// PSRAM. At grayscale_8bit a slot is ~0.5 MB, so the third one costs less
// than the two rgb565 slots it replaces.
void rotatePageSprites() {
  LGFX_Sprite *outgoing = gSpritePtr;
  gSpritePtr = nextPageSpritePtr;      // preloaded page goes live
  nextPageSpritePtr = prevPageSpritePtr;  // oldest slot is the next scratch
  prevPageSpritePtr = outgoing;        // keep the page we just left
}

void swapPrevPageSprite() { std::swap(gSpritePtr, prevPageSpritePtr); }
LGFX_Sprite menuCacheSprite(&M5.Display);
int lastDrawnMenuScroll = -1;
bool menuCacheValid = false;
std::string lastMangaPath = "";
int lastPage = 0;
std::string lastMangaName = "";

std::string preloadedMangaPath = "";
int preloadedPage = -1;
bool isNextPageReady = false;

std::vector<Bookmark> bookmarks;

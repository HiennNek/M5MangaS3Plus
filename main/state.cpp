#include "state.h"

#include <M5Unified.h>

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
DitherMode ditherMode = DITHER_FLOYD_STEINBERG;
ContrastPreset contrastPreset = CONTRAST_NORMAL;
bool isMagnifierActive = false;
int magnifierX = 0, magnifierY = 0;
std::string selectedBookmarkFolder = "";
epd_mode_t currentEpdMode = epd_mode_t::epd_fast;

LGFX_Sprite gSprite(&M5.Display);
LGFX_Sprite nextPageSprite(&M5.Display);
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

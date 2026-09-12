#include "state.h"
#include <M5Unified.h>

AppState appState = STATE_MENU;
std::vector<String> mangaFolders;
std::vector<int> mangaPageCounts;
int menuSelected = 0;
int menuScroll = 0;
int bookmarkScroll = 0;

String currentMangaPath;
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
String selectedBookmarkFolder = "";
epd_mode_t currentEpdMode = epd_mode_t::epd_fast;

Preferences prefs;
LGFX_Sprite gSprite(&M5.Display);
LGFX_Sprite nextPageSprite(&M5.Display);
LGFX_Sprite menuCacheSprite(&M5.Display);
int lastDrawnMenuScroll = -1;
bool menuCacheValid = false;
String lastMangaPath = "";
int lastPage = 0;
String lastMangaName = "";

String preloadedMangaPath = "";
int preloadedPage = -1;
bool isNextPageReady = false;

std::vector<Bookmark> bookmarks;

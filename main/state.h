#pragma once

#include "config.h"
#include <M5GFX.h>
#include <string>
#include <vector>

struct Bookmark
{
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
extern DitherMode ditherMode;
extern ContrastPreset contrastPreset;
extern bool isMagnifierActive;
extern int magnifierX, magnifierY;
extern std::string selectedBookmarkFolder;
extern epd_mode_t currentEpdMode;

extern LGFX_Sprite gSprite;
extern LGFX_Sprite nextPageSprite;
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

inline void requestRedraw(epd_mode_t mode = epd_mode_t::epd_fast)
{
  currentEpdMode = mode;
  needRedraw = true;
}

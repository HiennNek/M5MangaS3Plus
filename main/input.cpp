#include "input.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "bookmarks.h"
#include "compat.h"
#include "navigation.h"
#include "state.h"
#include "storage.h"
#include "ui.h"
#include "wifi_server.h"

// Brief visual flash on a button region to acknowledge a tap
static void flashButton(int x, int y, int w, int h, int radius = UI_RADIUS,
                        uint16_t color = UI_FG) {
  M5.Display.startWrite();
  M5.Display.fillRoundRect(x, y, w, h, radius, color);
  M5.Display.display();
  M5.Display.endWrite();
  idf_delay(50);
}

// Navigate the main menu to a new scroll position
static void menuNavigate(int newScroll) {
  menuScroll = newScroll;
  menuSelected = menuScroll;
  menuCacheValid = false;
  requestRedraw();
}

// Adjust the pending page in book-config by a delta, clamped to valid range
static void adjustPendingPage(int delta) {
  bookConfigPendingPage =
      std::max(0, std::min(totalPages - 1, bookConfigPendingPage + delta));
  requestRedraw();
}

// Pinch-zoom session: set once the spread exceeds ZOOM_ENGAGE.
static bool zoomEngaged = false;
// Render freshness while zoomed; shared with the no-touch poll path.
static bool qualityApplied = false;
static bool zoomSettleArmed = false;
static uint32_t zoomSettleTime = 0;

static void exitZoom(bool redraw) {
  if (!isZoomed) return;
  isZoomed = false;
  zoomFactor = 1.0f;
  zoomEngaged = false;
  zoomSettleArmed = false;
  if (redraw) needRedraw = true;
}

// Delayed single-tap: a quick tap waits DOUBLE_TAP_MS for a possible second
// tap (zoom entry, centered on it) before turning the page. Fired from the
// touch-idle path since it triggers with no fingers down.
static bool pendingTapArmed = false;
static uint32_t pendingTapTime = 0;
static int pendingTapX = 0, pendingTapY = 0;

static void disarmPendingTap() { pendingTapArmed = false; }

static void firePendingTap() {
  disarmPendingTap();
  if (pendingTapX >= LEFT_ZONE_W) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestRedraw(epd_mode_t::epd_quality);
      saveProgress();
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      requestRedraw(epd_mode_t::epd_quality);
      saveProgress();
    }
  }
}

static void pollPendingTap() {
  if (!pendingTapArmed) return;
  if (appState != STATE_READER || controlMenuOpen || bookConfigOpen ||
      isZoomed) {
    disarmPendingTap();
    return;
  }
  if (idf_millis() - pendingTapTime >= DOUBLE_TAP_MS) firePendingTap();
}

// A lone tap while zoomed must not render immediately: a quality refresh
// blocks far longer than DOUBLE_TAP_MS and would swallow the second tap,
// making zoom unexitable. Settle only once the window passes tap-free.
static void pollZoomSettle() {
  if (!zoomSettleArmed) return;
  if (!isZoomed || qualityApplied) {
    zoomSettleArmed = false;
    return;
  }
  if (idf_millis() - zoomSettleTime < DOUBLE_TAP_MS) return;
  zoomSettleArmed = false;
  drawZoomed(true);
  qualityApplied = true;
}

void handleTouch() {
  if (appState != STATE_READER) {
    // Reader overlays and other screens scribble over gSprite, which the
    // zoom view reads from — drop zoom while away. Also drop any pending
    // tap so it can't fire after returning.
    exitZoom(false);
    disarmPendingTap();
  }

  if (M5.Touch.getCount() == 0) {
    if (appState == STATE_READER && !controlMenuOpen && !bookConfigOpen) {
      pollPendingTap();
      pollZoomSettle();
    }
    return;
  }

  auto &t = M5.Touch.getDetail(0);

  if (!isZoomed && !controlMenuOpen && t.wasReleased() &&
      t.base_y < HEADER_H && t.distanceY() > SWIPE_UP_MIN) {
    disarmPendingTap();
    exitZoom(false);
    controlMenuOpen = true;
    requestRedraw();
    return;
  }

  if (controlMenuOpen)
    handleControlTouch(t);
  else if (bookConfigOpen)
    handleBookConfigTouch(t);
  else if (appState == STATE_MENU)
    handleMenuTouch(t);
  else if (appState == STATE_BOOKMARKS)
    handleBookmarksTouch(t);
  else if (appState == STATE_WIFI)
    handleWifiTouch(t);
  else {
    handleReaderTouch(t);
  }
}

void handleBookmarksTouch(const m5::touch_detail_t &t) {
  if (!t.wasReleased()) return;

  int dy = t.distanceY();
  int dx = t.distanceX();

  if (dy < -SWIPE_UP_MIN) {
    if (selectedBookmarkFolder == "") {
      appState = STATE_MENU;
    } else {
      selectedBookmarkFolder = "";
      bookmarkScroll = 0;
    }
    requestRedraw();
    return;
  }

  int itemsPerPage = (DISPLAY_H - 200) / 90;

  auto uniqueFolders = getUniqueBookmarkFolders();
  int totalItems;
  if (selectedBookmarkFolder == "") {
    totalItems = (int)uniqueFolders.size();
  } else {
    totalItems = 0;
    for (const auto &b : bookmarks) {
      if (b.folder == selectedBookmarkFolder) totalItems++;
    }
  }

  if (dy > SWIPE_UP_MIN || dx > SWIPE_HORIZ_MIN) {
    if (bookmarkScroll + itemsPerPage < totalItems) {
      bookmarkScroll += itemsPerPage;
    } else {
      bookmarkScroll = 0;
    }
    requestRedraw();
    return;
  }

  if (dx < -SWIPE_HORIZ_MIN) {
    if (bookmarkScroll > 0) {
      bookmarkScroll = std::max(0, bookmarkScroll - itemsPerPage);
    } else {
      bookmarkScroll = ((totalItems - 1) / itemsPerPage) * itemsPerPage;
    }
    requestRedraw();
    return;
  }

  int tapY = t.y;
  if (tapY < HEADER_H) return;

  int yOff = 100;
  int itemH = 80;

  if (selectedBookmarkFolder == "") {
    int count = 0;
    for (int i = 0; i < (int)uniqueFolders.size(); i++) {
      if (i < bookmarkScroll) continue;
      if (count >= itemsPerPage) break;

      if (tapY >= yOff && tapY <= yOff + itemH) {
        flashButton(10, yOff, DISPLAY_W - 20, itemH);
        selectedBookmarkFolder = uniqueFolders[i];
        bookmarkScroll = 0;
        requestRedraw();
        return;
      }
      yOff += itemH + 10;
      count++;
    }
  } else {
    int tapX = t.x;
    int count = 0;
    int folderItemIdx = 0;
    for (int i = 0; i < (int)bookmarks.size(); i++) {
      if (bookmarks[i].folder != selectedBookmarkFolder) continue;

      if (folderItemIdx < bookmarkScroll) {
        folderItemIdx++;
        continue;
      }
      if (count >= itemsPerPage) break;

      if (tapY >= yOff && tapY <= yOff + itemH) {
        if (tapX > DISPLAY_W - 120) {
          flashButton(DISPLAY_W - 100, yOff + 20, 70, 40);
          deleteBookmark(i);
          bool remains = false;
          for (const auto &b : bookmarks) {
            if (b.folder == selectedBookmarkFolder) {
              remains = true;
              break;
            }
          }
          if (!remains) {
            selectedBookmarkFolder = "";
            bookmarkScroll = 0;
          }
          requestRedraw();
          return;
        } else {
          flashButton(10, yOff, DISPLAY_W - 20, itemH);
          std::string path =
              std::string(MANGA_ROOT) + "/" + bookmarks[i].folder;
          openMangaPath(path, bookmarks[i].page);
          return;
        }
      }
      yOff += itemH + 10;
      count++;
      folderItemIdx++;
    }
  }
}

void handleWifiTouch(const m5::touch_detail_t &t) {
  if (!t.wasReleased()) return;

  int tapX = t.x;
  int tapY = t.y;

  int btnW = 300;
  int btnH = 60;
  int btnX = (DISPLAY_W - btnW) / 2;
  int btnY = DISPLAY_H - 150;

  if (tapX >= btnX && tapX <= btnX + btnW && tapY >= btnY &&
      tapY <= btnY + btnH) {
    M5.Display.startWrite();
    M5.Display.fillRoundRect(btnX, btnY, btnW, btnH, UI_RADIUS, UI_BG);
    M5.Display.drawRoundRect(btnX, btnY, btnW, btnH, UI_RADIUS, UI_BORDER);
    M5.Display.setTextColor(UI_FG, UI_BG);
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("STOP SERVER", btnX + btnW / 2, btnY + btnH / 2);
    M5.Display.setTextDatum(top_left);
    M5.Display.display();
    M5.Display.endWrite();
    idf_delay(50);

    stopWifiServer();
    appState = STATE_MENU;
    menuCacheValid = false;
    requestRedraw();
  }
}

void handleControlTouch(const m5::touch_detail_t &t) {
  if (!t.wasReleased()) return;
  if (t.distanceY() < -SWIPE_UP_MIN) {
    controlMenuOpen = false;
    requestRedraw((appState == STATE_READER) ? epd_mode_t::epd_quality
                                             : epd_mode_t::epd_fast);
    return;
  }

  int tapX = t.x;
  int tapY = t.y;
  int modW = 500;
  int modH = 300;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  int btnW = modW - 60;
  int btnH = 65;
  int btnX = modX + 30;
  int btnY = modY + 210;

  if (tapX > btnX && tapX < btnX + btnW && tapY > btnY && tapY < btnY + btnH) {
    M5.Display.startWrite();
    M5.Display.fillRoundRect(btnX, btnY, btnW, btnH, UI_RADIUS, UI_BG);
    M5.Display.drawRoundRect(btnX, btnY, btnW, btnH, UI_RADIUS, UI_BORDER);
    M5.Display.setTextColor(UI_FG, UI_BG);
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("SHUTDOWN", btnX + btnW / 2, btnY + btnH / 2);
    M5.Display.setTextDatum(top_left);
    M5.Display.display();
    M5.Display.endWrite();
    idf_delay(50);

    systemShutdown();
    return;
  }

  if (tapX < modX || tapX > modX + modW || tapY < modY || tapY > modY + modH) {
    controlMenuOpen = false;
    requestRedraw((appState == STATE_READER) ? epd_mode_t::epd_quality
                                             : epd_mode_t::epd_fast);
  }
}

void handleMenuTouch(const m5::touch_detail_t &t) {
  if (!t.wasReleased()) return;
  int dy = t.distanceY();
  int dx = t.distanceX();
  int totalItems = (int)mangaFolders.size() + 2;

  if (dx > SWIPE_HORIZ_MIN || dy > SWIPE_UP_MIN) {
    if (menuScroll + MENU_VISIBLE < totalItems) {
      menuNavigate(menuScroll + MENU_VISIBLE);
    } else if (dx > SWIPE_HORIZ_MIN) {
      menuNavigate(0);
    }
    return;
  }

  if (dx < -SWIPE_HORIZ_MIN) {
    if (menuScroll > 0) {
      menuNavigate(std::max(0, menuScroll - MENU_VISIBLE));
    } else {
      menuNavigate(((totalItems - 1) / MENU_VISIBLE) * MENU_VISIBLE);
    }
    return;
  }

  int tapX = t.x;
  int tapY = t.y;
  int barW = DISPLAY_W - 40;
  int barH = 60;
  int barX = 20;
  int barY = DISPLAY_H - 85;

  if (lastMangaName.length() > 0 && tapX >= barX && tapX <= barX + barW &&
      tapY >= barY && tapY <= barY + barH) {
    M5.Display.startWrite();
    M5.Display.fillRoundRect(barX, barY, barW, barH, UI_RADIUS, UI_FG);
    M5.Display.setTextColor(UI_BG, UI_FG);
    M5.Display.setFont(&fonts::DejaVu12);
    M5.Display.setCursor(barX + 15, barY + 12);
    M5.Display.print("CONTINUE: ");
    M5.Display.setFont(&fonts::DejaVu18);
    std::string shortName = displayName(lastMangaName);
    if (shortName.length() > 19) shortName = shortName.substr(0, 17) + "...";
    M5.Display.print(shortName.c_str());
    M5.Display.setFont(&fonts::DejaVu12);
    M5.Display.setCursor(barX + 15, barY + 36);
    M5.Display.printf("Page %d", lastPage + 1);
    M5.Display.display();
    M5.Display.endWrite();
    idf_delay(50);

    openMangaPath(lastMangaPath, lastPage);
    return;
  }
  if (tapY < GRID_Y_TOP) return;

  int col = (tapX - GRID_GUTTER) / (THUMB_W + GRID_GUTTER);
  int row = (tapY - GRID_Y_TOP) / GRID_ROW_H;
  if (col < 0) col = 0;
  if (col >= GRID_COLS) col = GRID_COLS - 1;
  if (row < 0) row = 0;
  if (row >= 2) row = 1;

  int idx = menuScroll + (row * GRID_COLS + col);
  if (idx >= totalItems) return;

  if (idx == menuSelected) {
    if (idx == 0) {
      appState = STATE_BOOKMARKS;
      selectedBookmarkFolder = "";
      bookmarkScroll = 0;
      requestRedraw();
    } else if (idx == 1) {
      appState = STATE_WIFI;
      requestRedraw();
    } else {
      openManga(idx - 2);
    }
  } else {
    menuSelected = idx;
    requestRedraw();
  }
}

void handleReaderTouch(const m5::touch_detail_t &t) {
  static uint32_t pressStart = 0;
  static uint32_t lastMoveTime = 0;
  static bool pinchActive = false;
  static float pinchBaseDist = 1.0f;
  static float pinchBaseZoom = 1.0f;
  static int lastPinchMX = 0, lastPinchMY = 0;
  static float panRemX = 0.0f, panRemY = 0.0f;
  static bool multiTouchPress = false;
  static uint32_t lastTapTime = 0;
  static int lastTapX = 0, lastTapY = 0;

  const uint32_t now = idf_millis();
  const int count = M5.Touch.getCount();

  // ---- two fingers: pinch adjusts an active zoom (double-tap enters) ----
  if (count >= 2) {
    multiTouchPress = true;
    disarmPendingTap();
    if (!isZoomed) return;
    const auto &a = M5.Touch.getDetail(0);
    const auto &b = M5.Touch.getDetail(1);
    const float dx = (float)a.x - (float)b.x;
    const float dy = (float)a.y - (float)b.y;
    const float dist = sqrtf(dx * dx + dy * dy);
    const int mx = (a.x + b.x) / 2;
    const int my = (a.y + b.y) / 2;

    if (!pinchActive) {
      pinchActive = true;
      pinchBaseDist = (dist > 1.0f) ? dist : 1.0f;
      pinchBaseZoom = zoomFactor;
      lastPinchMX = mx;
      lastPinchMY = my;
      panRemX = panRemY = 0.0f;
      lastMoveTime = now;
      qualityApplied = false;
      return;
    }

    if (dist > 1.0f) {
      float z = pinchBaseZoom * dist / pinchBaseDist;
      if (z < ZOOM_MIN) z = ZOOM_MIN;
      if (z > ZOOM_MAX) z = ZOOM_MAX;
      if (z > ZOOM_ENGAGE) zoomEngaged = true;
      if (fabsf(z - zoomFactor) > 0.005f || abs(mx - lastPinchMX) > 2 ||
          abs(my - lastPinchMY) > 2) {
        zoomFactor = z;
        zoomCX = mx;
        zoomCY = my;
        clampZoomViewport();
        lastPinchMX = zoomCX;
        lastPinchMY = zoomCY;
        drawZoomed(false);
        lastMoveTime = now;
        qualityApplied = false;
      }
    }

    // Pinch fully closed after a real zoom: back to the normal page.
    if (zoomEngaged && zoomFactor <= ZOOM_EXIT) {
      pinchActive = false;
      exitZoom(true);
    }
    return;
  }

  if (pinchActive) {
    // 2 -> 1 transition: end the pinch, anchor here to avoid a pan jump.
    pinchActive = false;
    lastMoveTime = now;
    qualityApplied = false;
  }

  // ---- one finger ----
  if (t.wasPressed()) {
    pressStart = now;
    lastMoveTime = now;
    qualityApplied = false;
    zoomSettleArmed = false;
    multiTouchPress = false;
    panRemX = panRemY = 0.0f;
  }

  if (t.isPressed() && isZoomed) {
    const int pdx = t.deltaX();
    const int pdy = t.deltaY();
    if (pdx != 0 || pdy != 0) {
      // Glide: content follows the finger 1:1 (page-space delta).
      panRemX += (float)pdx / zoomFactor;
      panRemY += (float)pdy / zoomFactor;
      const int stepX = (int)panRemX;
      const int stepY = (int)panRemY;
      panRemX -= (float)stepX;
      panRemY -= (float)stepY;
      if (stepX != 0 || stepY != 0) {
        zoomCX -= stepX;
        zoomCY -= stepY;
        clampZoomViewport();
        drawZoomed(false);
        lastMoveTime = now;
        qualityApplied = false;
      }
    } else if (!qualityApplied && now - lastMoveTime > ZOOM_SETTLE_MS) {
      drawZoomed(true);
      qualityApplied = true;
    }
  }

  if (t.wasReleased()) {
    const bool quickTap =
        !multiTouchPress && (now - pressStart < TAP_MAX_MS) &&
        abs(t.distanceX()) < TAP_SLOP_PX && abs(t.distanceY()) < TAP_SLOP_PX;
    multiTouchPress = false;

    if (isZoomed) {
      if (quickTap) {
        // Double-tap exits zoom; a lone tap just settles the render.
        if (now - lastTapTime < DOUBLE_TAP_MS &&
            abs(t.x - lastTapX) < TAP_SLOP_PX * 2 &&
            abs(t.y - lastTapY) < TAP_SLOP_PX * 2) {
          lastTapTime = 0;
          exitZoom(true);
        } else {
          lastTapTime = now;
          lastTapX = t.x;
          lastTapY = t.y;
          // No immediate redraw here (see pollZoomSettle): it would block
          // past the double-tap window and eat the exit tap.
          zoomSettleArmed = true;
          zoomSettleTime = now;
        }
      } else if (!zoomEngaged) {
        // Pinch that never spread: silently back to the normal page.
        exitZoom(true);
      } else if (!qualityApplied) {
        // Release after a glide: settle to quality, stay zoomed.
        drawZoomed(true);
        qualityApplied = true;
      }
      return;
    }
    // Not zoomed: book-config swipe, then tap (single: delayed page turn,
    // double: zoom in centered on the tap spot).
    if (t.base_y > DISPLAY_H - 150 && t.distanceY() < -SWIPE_UP_MIN) {
      disarmPendingTap();
      exitZoom(false);
      bookConfigOpen = true;
      bookConfigPendingPage = currentPage;
      requestRedraw();
      return;
    }

    if (!quickTap) {
      // Drags and slow presses cancel any tap still waiting on its double.
      disarmPendingTap();
      return;
    }
    if (pendingTapArmed && now - pendingTapTime < DOUBLE_TAP_MS &&
        abs(t.x - pendingTapX) < TAP_SLOP_PX * 2 &&
        abs(t.y - pendingTapY) < TAP_SLOP_PX * 2) {
      // Double-tap: enter zoom centered on the tap spot.
      disarmPendingTap();
      lastTapTime = 0;
      isZoomed = true;
      zoomEngaged = true;
      zoomFactor = ZOOM_DOUBLE_TAP;
      zoomCX = t.x;
      zoomCY = t.y;
      clampZoomViewport();
      drawZoomed(true);
      lastMoveTime = now;
      qualityApplied = true;
      return;
    }
    if (pendingTapArmed) {
      // Second tap elsewhere: the first tap was a lone turn, fire it now.
      firePendingTap();
    }
    pendingTapArmed = true;
    pendingTapTime = now;
    pendingTapX = t.x;
    pendingTapY = t.y;
  }
}
// Adjust the pending chapter by a delta, jumping to the first page of the
// neighboring chapter (clamped at the ends).
static void jumpToChapter(int delta) {
  const ChapterList &ch = getChapters(currentMangaPath);
  if (ch.names.empty()) return;
  int cur = chapterIndexForPage(currentMangaPath, bookConfigPendingPage);
  if (cur < 0) cur = 0;
  int nxt = std::max(0, std::min((int)ch.names.size() - 1, cur + delta));
  bookConfigPendingPage = ch.starts[(size_t)nxt];
  requestRedraw();
}

void handleBookConfigTouch(const m5::touch_detail_t &t) {
  if (!t.wasReleased()) return;

  int tapX = t.x;
  int tapY = t.y;
  int modW = BOOK_MOD_W;
  int modH = BOOK_MOD_H;
  int modX = (DISPLAY_W - modW) / 2;
  int modY = (DISPLAY_H - modH) / 2;

  if (tapX < modX || tapX > modX + modW || tapY < modY || tapY > modY + modH) {
    bookConfigOpen = false;
    currentPage = bookConfigPendingPage;
    requestRedraw(epd_mode_t::epd_quality);
    saveProgress(true);  // a << / >> / chapter jump was never persisted
    return;
  }

  int barY = modY + BOOK_PAGE_Y;
  if (tapY >= barY && tapY <= barY + BOOK_PAGE_H) {
    if (tapX < modX + 100)
      adjustPendingPage(-10);
    else if (tapX < modX + 180)
      adjustPendingPage(-1);
    else if (tapX > modX + modW - 100)
      adjustPendingPage(+10);
    else if (tapX > modX + modW - 180)
      adjustPendingPage(+1);
    return;
  }

  // Chapter stepper region (only active for multi-section CBZ).
  int chapY = modY + BOOK_CHAP_Y;
  if (tapY >= chapY && tapY <= chapY + BOOK_CHAP_H) {
    if (tapX < modX + BOOK_CHAP_ZONE)
      jumpToChapter(-1);
    else if (tapX > modX + modW - BOOK_CHAP_ZONE)
      jumpToChapter(+1);
    return;
  }

  int btnW = BOOK_BTN_W;
  int btnX = modX + BOOK_BTN_XOFF;
  int btnH = BOOK_BTN_H;

  int btnY0 = modY + BOOK_BTN_GRAY_Y;
  if (tapX >= btnX && tapX <= btnX + btnW && tapY >= btnY0 &&
      tapY <= btnY0 + btnH) {
    grayLevels = (grayLevels == 16) ? 8 : (grayLevels == 8) ? 4 : 16;
    if (grayLevels != 16) contrastPreset = CONTRAST_NORMAL;
    isNextPageReady = false;
    requestRedraw();
    return;
  }

  int btnY1 = modY + BOOK_BTN_CONTRAST_Y;
  if (tapX >= btnX && tapX <= btnX + btnW && tapY >= btnY1 &&
      tapY <= btnY1 + btnH) {
    if (grayLevels != 16) {
      M5.Display.startWrite();
      M5.Display.fillRoundRect(btnX, btnY1, btnW, btnH, UI_RADIUS, UI_FG);
      M5.Display.setTextColor(UI_BG, UI_FG);
      M5.Display.setFont(&fonts::DejaVu18);
      M5.Display.setTextDatum(middle_center);
      M5.Display.drawString("16 LEVELS ONLY", btnX + btnW / 2,
                            btnY1 + btnH / 2);
      M5.Display.setTextDatum(top_left);
      M5.Display.display();
      M5.Display.endWrite();
      idf_delay(50);
      needRedraw = true;
      return;
    }
    contrastPreset = (ContrastPreset)((contrastPreset + 1) % CONTRAST_COUNT);
    isNextPageReady = false;
    requestRedraw();
    return;
  }

  int btnY2 = modY + BOOK_BTN_BOOKMARK_Y;
  if (tapX >= btnX && tapX <= btnX + btnW && tapY >= btnY2 &&
      tapY <= btnY2 + btnH) {
    M5.Display.startWrite();
    M5.Display.fillRoundRect(btnX, btnY2, btnW, btnH, UI_RADIUS, UI_FG);
    M5.Display.setTextColor(UI_BG, UI_FG);
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("BOOKMARK SAVED!", btnX + btnW / 2, btnY2 + btnH / 2);
    M5.Display.setTextDatum(top_left);
    M5.Display.display();
    M5.Display.endWrite();
    idf_delay(50);

    size_t lastSlash = currentMangaPath.rfind('/');
    std::string folder = (lastSlash == std::string::npos)
                             ? currentMangaPath
                             : currentMangaPath.substr(lastSlash + 1);
    addBookmark(folder, bookConfigPendingPage);  // the page shown in the modal
    needRedraw = true;
    return;
  }

  int btnY3 = modY + BOOK_BTN_RETURN_Y;
  if (tapX >= btnX && tapX <= btnX + btnW && tapY >= btnY3 &&
      tapY <= btnY3 + btnH) {
    M5.Display.startWrite();
    M5.Display.fillRoundRect(btnX, btnY3, btnW, btnH, UI_RADIUS, UI_BG);
    M5.Display.drawRoundRect(btnX, btnY3, btnW, btnH, UI_RADIUS, UI_BORDER);
    M5.Display.setTextColor(UI_FG, UI_BG);
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("RETURN TO LIBRARY", btnX + btnW / 2,
                          btnY3 + btnH / 2);
    M5.Display.setTextDatum(top_left);
    M5.Display.display();
    M5.Display.endWrite();
    idf_delay(50);

    appState = STATE_MENU;
    bookConfigOpen = false;
    flushProgress();  // don't lose a throttled position when leaving the book
    requestRedraw();
    return;
  }
}

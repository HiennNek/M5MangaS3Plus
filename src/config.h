#pragma once

// A friendly reminder from a real human:
// If code formatter give out compilation error
// Just place SD.h before M5GFX.h
// It took me 10min to figure this out
// Fuck arduino framework
// (This is not AI generated)

#include <Arduino.h>
#include <SD.h>
#include <M5GFX.h>

// SD SPI pins (M5PaperS3)
#define SD_CS_PIN 47
#define SD_SCK_PIN 39
#define SD_MOSI_PIN 38
#define SD_MISO_PIN 40

// Display geometry
#define DISPLAY_W 540
#define DISPLAY_H 960
#define LEFT_ZONE_W (DISPLAY_W / 2)
#define SWIPE_UP_MIN 80

// Manga root
#define MANGA_ROOT "/manga"
#define PIC_ROOT "/pic"

// Filename pattern
#define IMG_PREFIX "m5_"
#define IMG_SUFFIX ".jpg"
#define IMG_DIGITS 4

// Menu layout
#define GRID_COLS 2
#define GRID_GUTTER 30
#define GRID_Y_TOP 110
#define THUMB_W ((DISPLAY_W - (GRID_COLS + 1) * GRID_GUTTER) / GRID_COLS)
#define THUMB_H (int(THUMB_W * 1.41))
#define GRID_ROW_H (THUMB_H + 60)
#define MENU_VISIBLE (GRID_COLS * 2)
#define UI_RADIUS 8

// High-contrast UI colors
#define UI_BG TFT_WHITE
#define UI_FG TFT_BLACK
#define UI_ACCENT \
  0xEEEE // Light gray for active/filled backgrounds (never text)
#define UI_BORDER TFT_BLACK
#define UI_SHADOW TFT_BLACK

#define MAG_SIZE 240
#define MAG_SCALE 2

// Touch gesture thresholds
#define HEADER_H 100       // Height of header/top area that intercepts gestures
#define SWIPE_HORIZ_MIN 60 // Minimum horizontal distance for a swipe
#define LONG_PRESS_MS 600  // Milliseconds to trigger long-press (magnifier)

enum DitherMode
{
  DITHER_OFF,
  DITHER_FLOYD_STEINBERG,
  DITHER_ATKINSON,
  DITHER_ORDERED,
  DITHER_COUNT // Must be last — used for cycling
};

enum ContrastPreset
{
  CONTRAST_NORMAL,
  CONTRAST_VIVID, // +20% contrast — good all-rounder for e-ink
  CONTRAST_HIGH,  // +40% contrast — for washed-out scans
  CONTRAST_LIGHT, // Brightness boost — for dark manga
  CONTRAST_COUNT  // Must be last — used for cycling
};

enum AppState
{
  STATE_MENU,
  STATE_READER,
  STATE_BOOKMARKS,
  STATE_WIFI
};

#pragma once

#include <M5Unified.h>

void handleTouch();
void handleMenuTouch(const m5::touch_detail_t &t);
void handleBookmarksTouch(const m5::touch_detail_t &t);
void handleWifiTouch(const m5::touch_detail_t &t);
void handleReaderTouch(const m5::touch_detail_t &t);
void handleControlTouch(const m5::touch_detail_t &t);
void handleBookConfigTouch(const m5::touch_detail_t &t);

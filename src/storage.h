#pragma once

#include "config.h"
#include "state.h"

void sdInit();
void scanMangaFolders();
String makePagePath(const String &folder, int n);
bool pageExists(const String &folder, int n);
int findTotalPages(const String &folder);

void saveProgress();
void loadProgress();
void updateLastMangaName();

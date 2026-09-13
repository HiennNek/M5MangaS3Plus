#pragma once

#include "config.h"
#include "state.h"
#include <string>

void sdInit();
void scanMangaFolders();
std::string makePagePath(const std::string &folder, int n);
bool pageExists(const std::string &folder, int n);
int findTotalPages(const std::string &folder);

void saveProgress();
void loadProgress();
void updateLastMangaName();

#pragma once

#include "state.h"

void loadBookmarks();
void saveBookmarks();
void addBookmark(const String &folder, int page);
void deleteBookmark(int idx);
std::vector<String> getUniqueBookmarkFolders();

#pragma once

#include <string>
#include <vector>

#include "state.h"

void loadBookmarks();
void saveBookmarks();
void addBookmark(const std::string &folder, int page);
void deleteBookmark(int idx);
std::vector<std::string> getUniqueBookmarkFolders();

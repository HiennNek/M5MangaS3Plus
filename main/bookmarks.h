#pragma once

#include "state.h"
#include <string>
#include <vector>

void loadBookmarks();
void saveBookmarks();
void addBookmark(const std::string &folder, int page);
void deleteBookmark(int idx);
std::vector<std::string> getUniqueBookmarkFolders();

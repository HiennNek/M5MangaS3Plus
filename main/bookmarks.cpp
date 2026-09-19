#include "bookmarks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "compat.h"
#include "esp_log.h"

static const char *TAG = "bookmarks";
static const char *kBookmarksPath = "/sdcard/manga/bookmarks.csv";

void loadBookmarks() {
  bookmarks.clear();
  FILE *f = fopen(kBookmarksPath, "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (bookmarks.size() >= 200) break;
    std::string s = line;
    str_trim(s);
    if (s.empty()) continue;
    // Split on the LAST comma: the page is a bare integer, so the last comma
    // is always the separator, while the title may itself contain commas
    // ("Berserk, Vol 1.cbz"). Splitting on the first one truncated such
    // titles and turned the rest of the name into page 0. Files written by
    // older builds (no commas in names) parse exactly as before.
    size_t comma = s.rfind(',');
    if (comma != std::string::npos && comma > 0) {
      Bookmark b;
      b.folder = s.substr(0, comma);
      b.page = atoi(s.c_str() + comma + 1);
      if (b.page < 0) b.page = 0;
      bookmarks.push_back(b);
    }
  }
  fclose(f);
}

void saveBookmarks() {
  FILE *f = fopen(kBookmarksPath, "w");
  if (!f) {
    ESP_LOGW(TAG, "cannot open bookmarks for write");
    return;
  }
  for (const auto &b : bookmarks) {
    fprintf(f, "%s,%d\n", b.folder.c_str(), b.page);
  }
  fclose(f);
}

void addBookmark(const std::string &folder, int page) {
  if (folder.empty()) return;
  for (const auto &b : bookmarks) {
    if (b.folder == folder && b.page == page) return;
  }
  bookmarks.push_back({folder, page});
  saveBookmarks();
}

void deleteBookmark(int idx) {
  if (idx < 0 || idx >= (int)bookmarks.size()) return;
  bookmarks.erase(bookmarks.begin() + idx);
  saveBookmarks();
}

std::vector<std::string> getUniqueBookmarkFolders() {
  std::vector<std::string> folders;
  for (const auto &b : bookmarks) {
    if (std::find(folders.begin(), folders.end(), b.folder) == folders.end()) {
      folders.push_back(b.folder);
    }
  }
  std::sort(folders.begin(), folders.end());
  return folders;
}

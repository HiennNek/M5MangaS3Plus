#include "bookmarks.h"

#include <algorithm>
#include <cstdio>

#include "compat.h"
#include "esp_log.h"

static const char *TAG = "manga_bookmarks";
static const char *BOOKMARKS_PATH = MANGA_ROOT "/bookmarks.csv";

void loadBookmarks()
{
  bookmarks.clear();
  FILE *f = fopen(BOOKMARKS_PATH, "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f))
  {
    if (bookmarks.size() >= 200)
      break;
    std::string s = str_trim(line);
    if (s.empty())
      continue;
    size_t comma = s.find(',');
    if (comma != std::string::npos && comma > 0)
    {
      Bookmark b;
      b.folder = s.substr(0, comma);
      b.page = atoi(s.c_str() + comma + 1);
      bookmarks.push_back(b);
    }
  }
  fclose(f);
}

void saveBookmarks()
{
  FILE *f = fopen(BOOKMARKS_PATH, "w");
  if (!f)
  {
    ESP_LOGW(TAG, "cannot write %s", BOOKMARKS_PATH);
    return;
  }
  for (const auto &b : bookmarks)
    fprintf(f, "%s,%d\n", b.folder.c_str(), b.page);
  fclose(f);
}

void addBookmark(const std::string &folder, int page)
{
  if (folder.empty())
    return;
  for (const auto &b : bookmarks)
  {
    if (b.folder == folder && b.page == page)
      return;
  }
  bookmarks.push_back({folder, page});
  saveBookmarks();
}

void deleteBookmark(int idx)
{
  if (idx < 0 || idx >= (int)bookmarks.size())
    return;
  bookmarks.erase(bookmarks.begin() + idx);
  saveBookmarks();
}

std::vector<std::string> getUniqueBookmarkFolders()
{
  std::vector<std::string> folders;
  for (const auto &b : bookmarks)
  {
    if (std::find(folders.begin(), folders.end(), b.folder) == folders.end())
      folders.push_back(b.folder);
  }
  std::sort(folders.begin(), folders.end());
  return folders;
}

#include "bookmarks.h"
#include <SD.h>

void loadBookmarks()
{
  bookmarks.clear();
  File f = SD.open("/manga/bookmarks.csv", FILE_READ);
  if (!f)
    return;
  while (f.available())
  {
    if (bookmarks.size() >= 200)
      break;
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;
    int comma = line.indexOf(',');
    if (comma > 0)
    {
      Bookmark b;
      b.folder = line.substring(0, comma);
      b.page = line.substring(comma + 1).toInt();
      bookmarks.push_back(b);
    }
  }
  f.close();
}

void saveBookmarks()
{
  File f = SD.open("/manga/bookmarks.csv", FILE_WRITE);
  if (!f)
    return;
  for (const auto &b : bookmarks)
  {
    f.printf("%s,%d\n", b.folder.c_str(), b.page);
  }
  f.close();
}

void addBookmark(const String &folder, int page)
{
  if (folder.length() == 0)
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

std::vector<String> getUniqueBookmarkFolders()
{
  std::vector<String> folders;
  for (const auto &b : bookmarks)
  {
    if (std::find(folders.begin(), folders.end(), b.folder) == folders.end())
    {
      folders.push_back(b.folder);
    }
  }
  std::sort(folders.begin(), folders.end());
  return folders;
}

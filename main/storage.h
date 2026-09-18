#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cbz.h"
#include "config.h"
#include "state.h"

void sdInit();
void scanMangaFolders();
std::string makePagePath(const std::string &folder, int n);
bool pageExists(const std::string &folder, int n);

// A manga entry is either a folder of m5_NNNN.jpg files or a .cbz archive.
bool isCbzPath(const std::string &mangaPath);

// Chapters: shared CbzChapters model, cached per book. Empty for folder
// manga and single-section archives (navigator hidden in that case).
using ChapterList = CbzChapters;
const ChapterList &getChapters(const std::string &mangaPath);
int chapterIndexForPage(const std::string &mangaPath, int page);
// Cover source identity for the thumbnail cache: the file whose bytes the
// cover is rendered from (m5_0000.jpg for folders, the archive for CBZ)
// plus its size+mtime. False when the book has no cover.
bool bookCoverSource(const std::string &entry, uint32_t &fsize,
                     uint32_t &fmtime);
// Library display name: strips a trailing .cbz so archives read as titles.
std::string displayName(const std::string &entry);
int findTotalPages(const std::string &mangaPath);

// Decoded-source bytes for one page, in RAM. Folder pages reuse the shared
// jpg buffer (owned=false); CBZ pages are extracted to fresh PSRAM
// (owned=true, release with freePageData). buf==nullptr means failure.
struct PageData {
  uint8_t *buf = nullptr;
  size_t size = 0;
  bool owned = false;
};
// Bulk read into a caller-owned buffer. Use instead of fread() for page-
// sized files: see the comment in storage.cpp for why stdio is slow here.
size_t readFileToBuffer(const char *path, uint8_t *dst, size_t size);
size_t loadFileToJpgBuffer(const char *path);
uint8_t *jpgSharedBuffer();  // buffer behind loadFileToJpgBuffer
PageData loadPageData(const std::string &mangaPath, int page);
void freePageData(PageData &p);

void saveProgress();
void loadProgress();
void updateLastMangaName();

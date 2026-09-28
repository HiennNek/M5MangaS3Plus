#pragma once
// Persistent per-book index.
//
// Opening a book used to re-parse a .cbz central directory (thousands of
// scattered reads) or re-probe a folder's page files on every boot. This
// module caches the expensive result under /sdcard/.index/ so the next open
// is one small sequential fread instead of a scan.
//
// A cache entry is trusted only while the book's identity is unchanged:
//   - CBZ: archive size + mtime (adding a chapter rewrites the file).
//   - Folder: directory mtime, plus the caller's boundary check (see
//     findTotalPages) which catches appends/removals even when FAT does not
//     refresh the directory mtime.
// Any mismatch makes index_load() report a miss and the caller re-indexes.
//
// Bump INDEX_MAGIC on any format change to invalidate old files.

#include <cstdint>
#include <string>
#include <vector>

#include "cbz.h"  // CbzArchive, PsramAlloc/PsramString

#define INDEX_DIR "/sdcard/.index"
#define INDEX_MAGIC "M5IDX001"
#define INDEX_MAX_FILES 256

enum BookIndexKind { BOOK_IDX_FOLDER = 0, BOOK_IDX_CBZ = 1 };

// One CBZ image entry. Empty for folder books (their pages follow the fixed
// m5_NNNN.jpg pattern, so the count alone is enough). The table lives in
// PSRAM like CbzArchive itself: a big archive has thousands of entries.
struct IndexPage {
  PsramString name;
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  uint32_t local_offset = 0;
  uint16_t method = 0;
};

struct BookIndex {
  uint32_t fsize = 0;    // identity of the source at save time
  uint32_t fmtime = 0;
  int page_count = 0;
  std::vector<IndexPage, PsramAlloc<IndexPage>> pages;  // CBZ only
};

// Deterministic cache path for a bare library entry name.
std::string index_path_for(const std::string &entry);
// Load the cache for `entry` (absolute `path` is re-stat'd to verify the
// identity). False = no usable cache; caller must re-index.
bool index_load(const std::string &entry, const std::string &path,
                BookIndexKind kind, BookIndex &out);
// Persist `in`, stamping it with `path`'s current identity. Best effort.
bool index_save(const std::string &entry, const std::string &path,
                BookIndexKind kind, const BookIndex &in);
// Delete the cache belonging to entry (book removed/renamed).
void index_purge_for(const std::string &entry);
// Bound total cached files (deletes oldest by mtime past the cap).
void index_maintain_cap();

// Open a .cbz through the index: restores the entry table from cache when
// valid, else parses the archive and saves a fresh index.
CbzArchive *openCbzWithIndex(const std::string &path);

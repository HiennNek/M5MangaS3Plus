#pragma once
// Minimal CBZ (ZIP archive) reader.
//
// Library entries are either folders of m5_NNNN.jpg files or *.cbz files.
// This module covers the CBZ side: hand-rolled EOCD + central-directory
// parsing, DEFLATE entries inflated with the ESP32-S3 ROM tinfl (no extra
// component needed). Only stored (method 0) and deflated (method 8)
// entries are supported; encrypted, multi-disk and ZIP64 entries are
// skipped. Image entries (jpg/jpeg/png, natural-sorted) become pages.
//
// Memory: a big archive holds thousands of entries, far more than fits in
// internal RAM, so entry names and the entry vector live in PSRAM via
// PsramAlloc. Allocation failures there are still fatal (no exceptions in
// firmware), so cbz_open() additionally budgets entries against free PSRAM
// and truncates gracefully instead of aborting.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "esp_heap_caps.h"

template <typename T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() = default;
  template <typename U>
  PsramAlloc(const PsramAlloc<U> &) {}
  T *allocate(std::size_t n) {
    if (void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM))
      return static_cast<T *>(p);
    // Unreachable in practice: cbz_open() budgets entries against free
    // PSRAM first. No exceptions in firmware, so abort like operator new.
    abort();
    return nullptr;
  }
  void deallocate(T *p, std::size_t) noexcept { heap_caps_free(p); }
};
template <typename T, typename U>
bool operator==(const PsramAlloc<T> &, const PsramAlloc<U> &) {
  return true;
}
template <typename T, typename U>
bool operator!=(const PsramAlloc<T> &, const PsramAlloc<U> &) {
  return false;
}

using PsramString = std::basic_string<char, std::char_traits<char>,
                                      PsramAlloc<char>>;

struct CbzEntry {
  PsramString name;       // full path inside the archive
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  uint32_t local_offset = 0;
  uint16_t method = 0;    // 0 = stored, 8 = deflated
};

struct CbzArchive {
  std::string path;
  FILE *f = nullptr;
  std::vector<CbzEntry, PsramAlloc<CbzEntry>> images;  // natural-sorted pages
};

// ".cbz" suffix, case-insensitive.
bool cbz_is_cbz_path(std::string_view path);

// Chapter grouping: consecutive pages sharing a top-level folder form a
// chapter ("Spook 42/001.png" -> "Spook 42", root files -> "(root)").
// Fewer than 2 distinct groups yields an empty list (no navigator).
struct CbzChapters {
  std::vector<std::string> names;  // chapter titles in page order
  std::vector<int> starts;         // first page index per chapter
};
CbzChapters cbz_chapters(const CbzArchive *a);

// Parse the central directory. Returns nullptr if the file is not a
// readable ZIP (missing/corrupt headers, multi-disk, ...). An archive
// with zero usable images still returns a valid (empty) handle.
CbzArchive *cbz_open(const std::string &path);
void cbz_close(CbzArchive *a);

// Extract entry idx to a fresh PSRAM buffer (*out_buf, caller frees with
// heap_caps_free()). Returns uncompressed size, 0 on any failure.
size_t cbz_extract(CbzArchive *a, size_t idx, uint8_t **out_buf);

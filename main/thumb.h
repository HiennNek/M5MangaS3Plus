#pragma once
// Per-book cover thumbnail cache.
//
// First menu visit decodes page 0 at full cost; afterwards the cover is a
// pre-fitted, pre-centered 8-bit grayscale raw dump of exactly
// THUMB_IMG_W x THUMB_IMG_H pixels behind a small header, loadable with a
// single fread + row memcpy (no JPEG/PNG decode at all).
//
// Layout: THUMB_DIR/thm_<fnv1a64(entry)hex>_<bytes>_<mtime>.raw
// The size+mtime in the name invalidates stale entries automatically;
// orphans are bounded by thumb_maintain_cap() and purged on book delete.
// Bump THUMB_MAGIC on any format change to invalidate old files.
//
// Deliberately free of ESP/M5GFX headers so it stays host-testable.

#include <cstddef>
#include <cstdint>
#include <string>

// Must match the frame inner box in ui.cpp (THUMB_W-4 x THUMB_H-4).
#define THUMB_IMG_W 221
#define THUMB_IMG_H 313
#define THUMB_DIR "/sdcard/.thumbs"
#define THUMB_MAGIC "M5THM002"
#define THUMB_MAX_FILES 256

struct ThumbHeader {
  char magic[8];
  uint16_t w;
  uint16_t h;
  uint8_t depth;  // always 8
  uint8_t reserved[3];
};
static_assert(sizeof(ThumbHeader) == 16, "thumb header must be 16 bytes");

uint64_t thumb_fnv1a64(const char *s);
// Deterministic cache path for a book entry + its cover source identity.
std::string thumb_key_for(const std::string &entry, uint32_t fsize,
                          uint32_t fmtime);
// Best-effort store (creates THUMB_DIR). False = keep direct-decode path.
bool thumb_save(const char *path, const uint8_t *px, int w, int h);
// Validated load into a w*h buffer. False = regenerate.
bool thumb_load(const char *path, uint8_t *px, int w, int h);
// Delete thumbnails belonging to entry (by name hash prefix).
void thumb_purge_for(const std::string &entry);
// Bound total cached files (deletes oldest by mtime past the cap).
void thumb_maintain_cap();

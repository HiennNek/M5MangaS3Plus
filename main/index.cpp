#include "index.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define INDEX_MKDIR(p) mkdir(p)
#else
#include <dirent.h>
#include <sys/types.h>
#define INDEX_MKDIR(p) mkdir(p, 0755)
#endif

#include "cbz.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "thumb.h"  // thumb_fnv1a64 (host-testable hash)

static const char *TAG = "index";

// Sanity caps on anything read back from disk.
static const uint32_t kMaxPages = 100000;
static const uint32_t kMaxName = 1024;

static const size_t kHeaderSize = 8 + 4 + 4 + 4 + 4;  // magic..count

static std::string base_name(const std::string &p) {
  size_t slash = p.rfind('/');
  return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

std::string index_path_for(const std::string &entry) {
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx",
           (unsigned long long)thumb_fnv1a64(entry.c_str()));
  return std::string(INDEX_DIR) + "/idx_" + hex + ".bin";
}

static bool stat_identity(const std::string &path, uint32_t &fsize,
                          uint32_t &fmtime) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0) return false;
  fsize = (uint32_t)st.st_size;
  fmtime = (uint32_t)st.st_mtime;
  return true;
}

static bool read_exact(FILE *f, void *dst, size_t n) {
  return fread(dst, 1, n, f) == n;
}

static bool write_exact(FILE *f, const void *src, size_t n) {
  return fwrite(src, 1, n, f) == n;
}

bool index_load(const std::string &entry, const std::string &path,
                BookIndexKind kind, BookIndex &out) {
  std::string p = index_path_for(entry);
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return false;

  char magic[8] = {};
  uint8_t kind_byte = 0, reserved[3] = {};
  uint32_t fsize = 0, fmtime = 0, count = 0;
  bool ok = read_exact(f, magic, sizeof(magic)) &&
            read_exact(f, &kind_byte, 1) && read_exact(f, reserved, 3) &&
            read_exact(f, &fsize, 4) && read_exact(f, &fmtime, 4) &&
            read_exact(f, &count, 4);
  if (ok) {
    ok = memcmp(magic, INDEX_MAGIC, sizeof(magic)) == 0 &&
         kind_byte == (uint8_t)kind && count <= kMaxPages;
  }
  // A corrupt count must not drive a huge PsramAlloc reserve (fatal, since
  // the allocator aborts). Budget like cbz_open(): ~128 B/entry heuristic.
  if (ok && kind == BOOK_IDX_CBZ) {
    size_t budget = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 2 / 128;
    if (count > budget) ok = false;
  }

  // Verify the source still matches the identity captured at save time.
  uint32_t cur_size = 0, cur_mtime = 0;
  if (ok && (!stat_identity(path, cur_size, cur_mtime) || cur_size != fsize ||
             cur_mtime != fmtime))
    ok = false;

  // Reject a stale file outright so the caller re-indexes and overwrites.
  if (!ok) {
    fclose(f);
    remove(p.c_str());
    return false;
  }

  BookIndex bi;
  bi.fsize = fsize;
  bi.fmtime = fmtime;
  bi.page_count = (int)count;
  if (kind == BOOK_IDX_CBZ) {
    bi.pages.reserve(count);
    for (uint32_t i = 0; i < count && ok; ++i) {
      uint32_t name_len = 0;
      IndexPage pg;
      uint16_t reserved16 = 0;
      ok = read_exact(f, &name_len, 4) &&
           read_exact(f, &pg.comp_size, 4) && read_exact(f, &pg.uncomp_size, 4) &&
           read_exact(f, &pg.local_offset, 4) && read_exact(f, &pg.method, 2) &&
           read_exact(f, &reserved16, 2) && name_len <= kMaxName;
      if (!ok) break;
      // PsramString has no resize(len, char), so build the temporary with
      // the plain std::string constructor, then copy into PSRAM.
      std::string tmp(name_len, '\0');
      if (name_len > 0 && !read_exact(f, tmp.data(), name_len)) {
        ok = false;
        break;
      }
      pg.name.assign(tmp.data(), tmp.size());
      bi.pages.push_back(std::move(pg));
    }
  }
  fclose(f);
  if (!ok) {
    remove(p.c_str());
    return false;
  }
  out = std::move(bi);
  return true;
}

bool index_save(const std::string &entry, const std::string &path,
                BookIndexKind kind, const BookIndex &in) {
  uint32_t fsize = 0, fmtime = 0;
  if (!stat_identity(path, fsize, fmtime)) return false;

  INDEX_MKDIR(INDEX_DIR);  // idempotent
  std::string p = index_path_for(entry);
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) return false;

  uint8_t kind_byte = (uint8_t)kind;
  uint8_t reserved[3] = {};
  uint32_t count = (uint32_t)in.page_count;
  bool ok = write_exact(f, INDEX_MAGIC, 8) && write_exact(f, &kind_byte, 1) &&
            write_exact(f, reserved, 3) && write_exact(f, &fsize, 4) &&
            write_exact(f, &fmtime, 4) && write_exact(f, &count, 4);
  if (ok && kind == BOOK_IDX_CBZ) {
    for (const IndexPage &pg : in.pages) {
      uint32_t name_len = (uint32_t)pg.name.size();
      uint16_t reserved16 = 0;
      if (!write_exact(f, &name_len, 4) || !write_exact(f, &pg.comp_size, 4) ||
          !write_exact(f, &pg.uncomp_size, 4) ||
          !write_exact(f, &pg.local_offset, 4) ||
          !write_exact(f, &pg.method, 2) || !write_exact(f, &reserved16, 2) ||
          (name_len > 0 && !write_exact(f, pg.name.data(), name_len))) {
        ok = false;
        break;
      }
    }
  }
  if (ok) ok = (fclose(f) == 0);
  if (!ok) {
    if (f) fclose(f);
    remove(p.c_str());  // never leave a partial index behind
    return false;
  }
  return true;
}

void index_purge_for(const std::string &entry) {
  std::string p = index_path_for(entry);
  remove(p.c_str());
}

void index_maintain_cap() {
  DIR *d = opendir(INDEX_DIR);
  if (!d) return;
  struct entry_info {
    std::string path;
    long mtime;
  };
  std::vector<entry_info> files;
  struct dirent *e;
  size_t scanned = 0;
  while ((e = readdir(d)) != nullptr) {
    if (++scanned > 4096) break;  // bound memory on hostile directories
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string full = std::string(INDEX_DIR) + "/" + n;
    struct stat st = {};
    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      files.push_back({full, (long)st.st_mtime});
  }
  closedir(d);
  if (files.size() <= INDEX_MAX_FILES) return;
  std::sort(files.begin(), files.end(),
            [](const entry_info &a, const entry_info &b) {
              return a.mtime < b.mtime;  // oldest first
            });
  for (size_t i = 0; i + INDEX_MAX_FILES < files.size(); i++)
    remove(files[i].path.c_str());
}

CbzArchive *openCbzWithIndex(const std::string &path) {
  std::string entry = base_name(path);
  BookIndex bi;
  if (index_load(entry, path, BOOK_IDX_CBZ, bi)) {
    CbzArchive *a = new (std::nothrow) CbzArchive();
    if (a) {
      a->path = path;
      a->f = fopen(path.c_str(), "rb");
      if (!a->f) {
        delete a;
        a = nullptr;
      } else {
        a->images.reserve(bi.pages.size());
        // Move each name across (same PsramString type): no duplicate of the
        // name bytes, so a huge index does not need 2x PSRAM while loading.
        for (IndexPage &pg : bi.pages) {
          CbzEntry en;
          en.name = std::move(pg.name);
          en.comp_size = pg.comp_size;
          en.uncomp_size = pg.uncomp_size;
          en.local_offset = pg.local_offset;
          en.method = pg.method;
          a->images.push_back(std::move(en));
        }
        ESP_LOGI(TAG, "indexed open %s: %d pages", entry.c_str(),
                 (int)a->images.size());
        return a;
      }
    }
  }

  // Miss or corrupt: parse once, then persist for next time.
  CbzArchive *a = cbz_open(path);
  if (a) {
    BookIndex fresh;
    fresh.page_count = (int)a->images.size();
    fresh.pages.reserve(a->images.size());
    for (const CbzEntry &en : a->images) {
      IndexPage pg;
      pg.name.assign(en.name.data(), en.name.size());
      pg.comp_size = en.comp_size;
      pg.uncomp_size = en.uncomp_size;
      pg.local_offset = en.local_offset;
      pg.method = en.method;
      fresh.pages.push_back(std::move(pg));
    }
    if (index_save(entry, path, BOOK_IDX_CBZ, fresh)) index_maintain_cap();
  }
  return a;
}

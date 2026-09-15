#include "thumb.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define THUMB_MKDIR(p) mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#define THUMB_MKDIR(p) mkdir(p, 0755)
#endif

uint64_t thumb_fnv1a64(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string thumb_key_for(const std::string &entry, uint32_t fsize,
                          uint32_t fmtime) {
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx",
           (unsigned long long)thumb_fnv1a64(entry.c_str()));
  char buf[160];
  snprintf(buf, sizeof(buf), "%s/thm_%s_%u_%u.raw", THUMB_DIR, hex,
           (unsigned)fsize, (unsigned)fmtime);
  return std::string(buf);
}

static bool thumb_header_ok(const ThumbHeader &hd, int w, int h) {
  return memcmp(hd.magic, THUMB_MAGIC, sizeof(hd.magic)) == 0 &&
         hd.w == (uint16_t)w && hd.h == (uint16_t)h && hd.depth == 8;
}

bool thumb_save(const char *path, const uint8_t *px, int w, int h) {
  if (!px || w <= 0 || h <= 0) return false;
  THUMB_MKDIR(THUMB_DIR);  // idempotent; ignored when present
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  ThumbHeader hd = {};
  memcpy(hd.magic, THUMB_MAGIC, sizeof(hd.magic));
  hd.w = (uint16_t)w;
  hd.h = (uint16_t)h;
  hd.depth = 8;
  size_t pxn = (size_t)w * (size_t)h;
  bool ok = fwrite(&hd, 1, sizeof(hd), f) == sizeof(hd) &&
            fwrite(px, 1, pxn, f) == pxn;
  if (ok)
    ok = (fclose(f) == 0);
  else
    fclose(f);
  if (!ok) {
    remove(path);  // never leave a partial file behind
    return false;
  }
  return true;
}

bool thumb_load(const char *path, uint8_t *px, int w, int h) {
  if (!px || w <= 0 || h <= 0) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  ThumbHeader hd = {};
  size_t pxn = (size_t)w * (size_t)h;
  bool ok = fread(&hd, 1, sizeof(hd), f) == sizeof(hd) &&
            thumb_header_ok(hd, w, h) &&
            fread(px, 1, pxn, f) == pxn;
  // Exact-size match only: anything appended or missing invalidates.
  int tail = fgetc(f);
  if (tail != EOF) ok = false;
  fclose(f);
  return ok;
}

void thumb_purge_for(const std::string &entry) {
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx",
           (unsigned long long)thumb_fnv1a64(entry.c_str()));
  std::string prefix = std::string("thm_") + hex + "_";
  DIR *d = opendir(THUMB_DIR);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n.compare(0, prefix.size(), prefix) == 0) {
      std::string full = std::string(THUMB_DIR) + "/" + n;
      remove(full.c_str());
    }
  }
  closedir(d);
}

void thumb_maintain_cap() {
  DIR *d = opendir(THUMB_DIR);
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
    std::string full = std::string(THUMB_DIR) + "/" + n;
    struct stat st = {};
    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      files.push_back({full, (long)st.st_mtime});
  }
  closedir(d);
  if (files.size() <= THUMB_MAX_FILES) return;
  std::sort(files.begin(), files.end(), [](const entry_info &a,
                                           const entry_info &b) {
    return a.mtime < b.mtime;  // oldest first
  });
  for (size_t i = 0; i + THUMB_MAX_FILES < files.size(); i++)
    remove(files[i].path.c_str());
}

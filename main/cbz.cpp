#include "cbz.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"  // ROM tinfl (esp_rom component, no archive APIs used)

static const char *TAG = "cbz";

// Sanity caps: refuse absurd entries instead of OOMing PSRAM.
static const size_t kMaxEntries = 100000;
static const size_t kMaxImages = 20000;  // collected pages; far above any manga
static const uint32_t kMaxUncompSize = 32u * 1024u * 1024u;
static const uint32_t kMaxCompSize = 64u * 1024u * 1024u;

static const uint32_t kSigLocal = 0x04034b50;
static const uint32_t kSigCentral = 0x02014b50;
static const uint32_t kSigEocd = 0x06054b50;

static bool read_u16(FILE *f, uint16_t *out) {
  uint8_t b[2];
  if (fread(b, 1, 2, f) != 2) return false;
  *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool read_u32(FILE *f, uint32_t *out) {
  uint8_t b[4];
  if (fread(b, 1, 4, f) != 4) return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
  return true;
}

static bool read_sig(FILE *f, uint32_t expect) {
  uint32_t sig = 0;
  return read_u32(f, &sig) && sig == expect;
}

bool cbz_is_cbz_path(std::string_view path) {
  if (path.size() < 4) return false;
  size_t n = path.size();
  return tolower((unsigned char)path[n - 4]) == '.' &&
         tolower((unsigned char)path[n - 3]) == 'c' &&
         tolower((unsigned char)path[n - 2]) == 'b' &&
         tolower((unsigned char)path[n - 1]) == 'z';
}

static bool is_image_name(std::string_view name) {
  if (!name.empty() && name.back() == '/') return false;  // directory
  if (name.find("__MACOSX") != std::string_view::npos) return false;
  size_t slash = name.rfind('/');
  std::string_view base =
      (slash == std::string_view::npos) ? name : name.substr(slash + 1);
  if (!base.empty() && base[0] == '.') return false;  // .DS_Store etc.
  size_t dot = base.rfind('.');
  if (dot == std::string_view::npos) return false;
  std::string ext(base.substr(dot));
  for (auto &c : ext) c = (char)tolower((unsigned char)c);
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

// Case-insensitive natural order: "2.jpg" < "10.jpg".
static bool natural_less(std::string_view a, std::string_view b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
    if (isdigit(ca) && isdigit(cb)) {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && a[i2] == '0') i2++;
      while (j2 < b.size() && b[j2] == '0') j2++;
      size_t e1 = i2;
      while (e1 < a.size() && isdigit((unsigned char)a[e1])) e1++;
      size_t e2 = j2;
      while (e2 < b.size() && isdigit((unsigned char)b[e2])) e2++;
      size_t l1 = e1 - i2, l2 = e2 - j2;
      if (l1 != l2) return l1 < l2;
      int c = a.compare(i2, l1, b, j2, l2);
      if (c != 0) return c < 0;
      while (i < a.size() && isdigit((unsigned char)a[i])) i++;
      while (j < b.size() && isdigit((unsigned char)b[j])) j++;
      continue;
    }
    int d = tolower(ca) - tolower(cb);
    if (d != 0) return d < 0;
    i++;
    j++;
  }
  return (a.size() - i) < (b.size() - j);
}

CbzArchive *cbz_open(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return nullptr;
  }
  long file_size = ftell(f);
  if (file_size < 22) {  // smaller than a bare EOCD
    fclose(f);
    return nullptr;
  }

  // EOCD is the last 22 bytes + up to 64KB comment. Scan backwards.
  // Scratch lives in PSRAM: 64KB of internal RAM is not guaranteed
  // while reader sprites and WiFi are up.
  long scan_len = file_size < (long)(22 + 65535) ? file_size : (long)(22 + 65535);
  uint8_t *tail =
      (uint8_t *)heap_caps_malloc((size_t)scan_len, MALLOC_CAP_SPIRAM);
  if (!tail) {
    fclose(f);
    return nullptr;
  }
  if (fseek(f, file_size - scan_len, SEEK_SET) != 0 ||
      fread(tail, 1, (size_t)scan_len, f) != (size_t)scan_len) {
    heap_caps_free(tail);
    fclose(f);
    return nullptr;
  }
  long eocd_off = -1;
  for (long i = scan_len - 22; i >= 0; --i) {
    if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 &&
        tail[i + 3] == 0x06) {
      eocd_off = i;
      break;
    }
  }
  if (eocd_off < 0) {  // not a ZIP (EOCD signature missing)
    heap_caps_free(tail);
    fclose(f);
    return nullptr;
  }
  const uint8_t *e = tail + eocd_off;
  uint16_t disk_no = e[4] | ((uint16_t)e[5] << 8);
  uint16_t cd_disk = e[6] | ((uint16_t)e[7] << 8);
  uint16_t entries = e[8] | ((uint16_t)e[9] << 8);
  uint32_t cd_size =
      (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) |
      ((uint32_t)e[15] << 24);
  uint32_t cd_offset =
      (uint32_t)e[16] | ((uint32_t)e[17] << 8) | ((uint32_t)e[18] << 16) |
      ((uint32_t)e[19] << 24);
  heap_caps_free(tail);

  if (disk_no != 0 || cd_disk != 0) {
    ESP_LOGW(TAG, "multi-disk archive unsupported: %s", path.c_str());
    fclose(f);
    return nullptr;
  }
  if (entries > kMaxEntries || (uint64_t)cd_offset + cd_size > (uint64_t)file_size) {
    ESP_LOGW(TAG, "bad central directory: %s", path.c_str());
    fclose(f);
    return nullptr;
  }

  CbzArchive *a = new (std::nothrow) CbzArchive();
  if (!a) {
    fclose(f);
    return nullptr;
  }
  a->path = path;
  a->f = f;

  // Budget entries against free PSRAM (~128B/entry heuristic for the struct
  // plus a typical name). Internal RAM cannot hold thousands of entries, and
  // allocation failure would abort (no exceptions), so truncate gracefully.
  size_t entry_cap = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 2 / 128;
  if (entry_cap < 100) entry_cap = 100;
  if (entry_cap > kMaxImages) entry_cap = kMaxImages;
  if (entry_cap > entries) entry_cap = entries;
  a->images.reserve(entry_cap);

  if (fseek(f, cd_offset, SEEK_SET) != 0) {
    cbz_close(a);
    return nullptr;
  }
  std::string name;
  for (uint16_t n = 0; n < entries; ++n) {
    if (a->images.size() >= entry_cap) {
      ESP_LOGW(TAG, "truncating %s at %d pages (PSRAM budget)", path.c_str(),
               (int)entry_cap);
      break;
    }
    if (!read_sig(f, kSigCentral)) break;
    uint16_t flags, method;
    uint32_t comp_size, uncomp_size, local_offset;
    uint16_t name_len, extra_len, comment_len;
    if (fseek(f, 4, SEEK_CUR) != 0) break;              // version made/needed
    if (!read_u16(f, &flags) || !read_u16(f, &method)) break;
    if (fseek(f, 8, SEEK_CUR) != 0) break;              // time/date/crc
    if (!read_u32(f, &comp_size) || !read_u32(f, &uncomp_size)) break;
    if (!read_u16(f, &name_len) || !read_u16(f, &extra_len) ||
        !read_u16(f, &comment_len))
      break;
    if (fseek(f, 8, SEEK_CUR) != 0) break;              // disk/int/ext attrs
    if (!read_u32(f, &local_offset)) break;
    if (name_len > 1024) {  // oversize name: skip entry, keep parsing
      if (fseek(f, (long)name_len + extra_len + comment_len, SEEK_CUR) != 0)
        break;
      continue;
    }
    if ((uint64_t)local_offset >= (uint64_t)file_size) continue;
    name.assign(name_len, '\0');
    if (name_len > 0 && fread(name.data(), 1, name_len, f) != name_len) break;
    if (fseek(f, extra_len + comment_len, SEEK_CUR) != 0) break;

    if ((flags & 0x01) != 0) continue;                  // encrypted
    if (method != 0 && method != 8) continue;           // stored/deflated only
    if (comp_size == 0xFFFFFFFF || uncomp_size == 0xFFFFFFFF ||
        local_offset == 0xFFFFFFFF)
      continue;                                         // ZIP64, unsupported
    if (uncomp_size == 0 || uncomp_size > kMaxUncompSize) continue;
    if (comp_size == 0 || comp_size > kMaxCompSize) continue;
    if (!is_image_name(name)) continue;
    CbzEntry en;
    en.name.assign(name.data(), name.size());
    en.comp_size = comp_size;
    en.uncomp_size = uncomp_size;
    en.local_offset = local_offset;
    en.method = method;
    a->images.push_back(en);
  }

  std::sort(a->images.begin(), a->images.end(),
            [](const CbzEntry &x, const CbzEntry &y) {
              return natural_less(x.name, y.name);
            });
  ESP_LOGI(TAG, "opened %s: %d pages", path.c_str(), (int)a->images.size());
  return a;
}

void cbz_close(CbzArchive *a) {
  if (!a) return;
  if (a->f) fclose(a->f);
  delete a;
}

CbzChapters cbz_chapters(const CbzArchive *a) {
  CbzChapters ch;
  if (!a) return ch;
  std::string lastKey;
  bool first = true;
  for (size_t i = 0; i < a->images.size(); ++i) {
    std::string_view nm = a->images[i].name;
    size_t slash = nm.find('/');
    std::string_view key =
        (slash == std::string_view::npos) ? std::string_view() : nm.substr(0, slash);
    if (first || key != std::string_view(lastKey)) {
      ch.names.push_back(key.empty() ? std::string("(root)") : std::string(key));
      ch.starts.push_back((int)i);
      lastKey.assign(key.data(), key.size());
      first = false;
    }
  }
  // Single section -> no navigator.
  if (ch.names.size() < 2) {
    ch.names.clear();
    ch.starts.clear();
  }
  return ch;
}

size_t cbz_extract(CbzArchive *a, size_t idx, uint8_t **out_buf) {
  *out_buf = nullptr;
  if (!a || !a->f || idx >= a->images.size()) return 0;
  const CbzEntry &en = a->images[idx];

  uint8_t *out =
      (uint8_t *)heap_caps_malloc(en.uncomp_size, MALLOC_CAP_SPIRAM);
  if (!out) {
    ESP_LOGW(TAG, "OOM extracting %s [%d]", en.name.c_str(), (int)idx);
    return 0;
  }

  if (fseek(a->f, (long)en.local_offset, SEEK_SET) != 0 ||
      !read_sig(a->f, kSigLocal)) {
    heap_caps_free(out);
    return 0;
  }
  if (fseek(a->f, 22, SEEK_CUR) != 0) {  // to name/extra lengths
    heap_caps_free(out);
    return 0;
  }
  uint16_t name_len, extra_len;
  if (!read_u16(a->f, &name_len) || !read_u16(a->f, &extra_len)) {
    heap_caps_free(out);
    return 0;
  }
  if (fseek(a->f, name_len + extra_len, SEEK_CUR) != 0) {
    heap_caps_free(out);
    return 0;
  }

  bool ok = false;
  if (en.method == 0) {  // stored
    ok = (en.comp_size == en.uncomp_size &&
          fread(out, 1, en.uncomp_size, a->f) == en.uncomp_size);
  } else {  // deflated: whole stream through ROM tinfl (raw, no zlib header)
    uint8_t *comp =
        (uint8_t *)heap_caps_malloc(en.comp_size, MALLOC_CAP_SPIRAM);
    if (comp) {
      if (fread(comp, 1, en.comp_size, a->f) == en.comp_size) {
        size_t got = tinfl_decompress_mem_to_mem(
            out, en.uncomp_size, comp, en.comp_size,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        ok = (got == en.uncomp_size);
      }
      heap_caps_free(comp);
    }
  }

  if (!ok) {
    ESP_LOGW(TAG, "extract failed: %s [%d]", en.name.c_str(), (int)idx);
    heap_caps_free(out);
    return 0;
  }
  *out_buf = out;
  return en.uncomp_size;
}

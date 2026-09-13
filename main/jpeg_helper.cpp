#include "jpeg_helper.h"

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "manga_jpg";

bool load_file_to_ram(const std::string &path, uint8_t **out_buf,
                      size_t *out_size)
{
  *out_buf = nullptr;
  if (out_size)
    *out_size = 0;

  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
  {
    ESP_LOGW(TAG, "open failed: %s", path.c_str());
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return false;
  }
  long len = ftell(f);
  rewind(f);
  if (len <= 0)
  {
    fclose(f);
    return false;
  }

  uint8_t *buf =
      (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
  if (!buf)
  {
    // Fall back to internal RAM for tiny files (thumbnails).
    buf = (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_INTERNAL);
  }
  if (!buf)
  {
    ESP_LOGE(TAG, "OOM loading %s (%ld bytes)", path.c_str(), len);
    fclose(f);
    return false;
  }

  size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len)
  {
    ESP_LOGW(TAG, "short read %s", path.c_str());
    heap_caps_free(buf);
    return false;
  }

  *out_buf = buf;
  if (out_size)
    *out_size = (size_t)len;
  return true;
}

bool draw_jpg_file(LGFX_Sprite &spr, const std::string &path, int32_t x,
                   int32_t y, int32_t w, int32_t h)
{
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (!load_file_to_ram(path, &buf, &len))
    return false;
  bool ok = spr.drawJpg(buf, len, x, y, w, h);
  heap_caps_free(buf);
  if (!ok)
    ESP_LOGW(TAG, "decode failed: %s", path.c_str());
  return ok;
}

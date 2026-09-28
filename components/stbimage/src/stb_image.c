// Compile unit for stb_image — software fallback for oversized pages.
//
// See ../CMakeLists.txt for why this exists. Keep the config minimal:
//   - No stdio (we decode from RAM buffers only).
//   - JPEG + PNG only; BMP/PSD/TGA/GIF/HDR/PIC/PNM compiled out.
//   - All heap use goes to PSRAM so large frames don't exhaust DRAM.

#include "esp_heap_caps.h"

// Route stb allocations to PSRAM. heap_caps_free() backs stbi_image_free().
#define STBI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, newsz) heap_caps_realloc((p), (newsz), MALLOC_CAP_SPIRAM)
#define STBI_FREE(p) heap_caps_free(p)

// 16-byte-aligned PSRAM allocation for JPEG component planes. stb_image.h
// uses it so data == raw_data holds and the luma plane can be handed back
// as the load result: no second full-size buffer and no byte-by-byte row
// copy of the whole frame (a multi-MB PSRAM round trip). Falls back to a
// plain allocation, which just keeps stb's copy path. heap_caps_free()
// frees aligned pointers, and component planes are never realloc'd.
static void *stbiPsramAligned(size_t size, size_t align) {
  void *p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
#define STBI_MALLOC_ALIGNED(sz, align) stbiPsramAligned((sz), (align))

#define STBI_NO_STDIO
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

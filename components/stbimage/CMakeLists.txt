# stb_image (nothings/stb, public domain / MIT, see src/stb_image.h).
#
# Software fallback decoder for oversized pages only. The fast paths stay on
# JPEGDEC (JPEG) and M5GFX (PNG); images larger than the target sprite go
# through stb_image, which decodes the full frame to grayscale in PSRAM and
# lets ui.cpp scale it to fit with aspect preservation. stb_image handles
# baseline + progressive JPEG and PNG reliably where JPEGDEC's power-of-two
# DCT scaling and M5GFX's affine scaler do not.
#
# Only JPEG+PNG are compiled in (STBI_NO_* for the rest) to keep flash down.
# Allocations are routed to PSRAM: a 667x1200 page is ~0.8 MB grayscale, and
# larger pages would exhaust internal RAM.
#
# Local patches to the vendored src/stb_image.h (output stays bit-identical
# to upstream, only the work done changes):
#   - 1-channel YCbCr JPEG loads decode luma only, skipping chroma plane
#     allocation, IDCT and dequant -- the same trick JPEGDEC's JPEG_LUMA_ONLY
#     uses on the fast path.
#   - 16-byte-aligned component planes let the luma plane be returned as the
#     load result instead of being copied into a second frame buffer.
#   - DC-only 8x8 blocks (flat areas, common in line art) fill directly.
#   - stbi_load_from_memory_half() decodes baseline JPEG at half resolution
#     with a 4x4-output IDCT (ported from libjpeg's jidctred.c, bit-exact):
#     ~2.4x less IDCT work and a 4x smaller PSRAM frame. ui.cpp uses it when
#     the page is at least 2x the target, so the fitted result is unchanged.

idf_component_register(
    SRCS
        "src/stb_image.c"
    INCLUDE_DIRS
        "src"
    PRIV_REQUIRES
        heap
)

# No -O3 here, unlike jpegdec. At -O3 xtensa-gcc inlines the whole JPEG
# pipeline into stbi_load_from_memory and its frame grows from 224 to 2320 B
# (stbi__parse_png_file: 1312 -> 5360 B). Oversized pages decode on the main
# task, whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE = 3584 B, so that
# overflows it; at -O2 every frame matches upstream and the local patches
# already provide the speedup. -O3 also costs ~320 KB of flash for ~3%.

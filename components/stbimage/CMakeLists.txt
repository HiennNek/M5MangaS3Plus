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

idf_component_register(
    SRCS
        "src/stb_image.c"
    INCLUDE_DIRS
        "src"
    PRIV_REQUIRES
        heap
)

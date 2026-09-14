#!/usr/bin/env python3
"""Kindle-style EPD quality waveform for M5PaperS3 (M5GFX Panel_EPD).

Background: stock M5GFX drives every epd_quality refresh as ~36 full panel
scans: eraser shuffle (4) + pre-drive (1) + white flash (2) + black flash (2)
+ 10 vendor gray rows + 16 NOP settle holds + end markers. Commercial readers
(Kindle etc.) instead do a simple black flash -> white -> image.

What this does: replaces ONLY the two builtin LUT data tables in the managed
M5GFX copy with a short Kindle-like script. No engine logic is touched, so
the step/retire machinery (step_quality, blit_dmabuf, offset tables, sizeof
step counts) keeps working unchanged:

  lut_eraser  (indexed by OLD pixels, runs first):
      2x all-black drive + end marker (3 scans; was 4).
      Uniform flash: every level is driven to black identically, fully
      clearing previous content. The end marker hands pixels to the pending
      (new image) pointer.
  lut_quality (indexed by NEW pixels, runs second):
      2x all-white flash + the 10 stock vendor gray rows verbatim +
      2 settle holds + end marker (15 scans; was 32).

Result per quality refresh: ~18 scans (~1.2s) with a black -> white -> image
sequence, instead of ~36 scans (~2.4s) of flicker.

Tuning (edit NEW_QUALITY below, then rebuild):
  - more all-white rows = cleaner whites, slower.
  - more ~0u hold rows = less ghosting on later fast updates, slower.
  - the 10 image rows are the vendor-tuned 16-gray convergence; trimming
    them posterizes grays (manga screentones suffer first).

Safety:
- Runs at CMake configure time via main/CMakeLists.txt (hook there).
- Pinned by SHA256 to the exact upstream file it was written for
  (M5GFX 0.2.29, see dependencies.lock). Any M5GFX update changes the hash
  -> configure stops with a loud error instead of silently building a
  mismatched tree. Re-verify the LUT block and update EXPECTED_PRISTINE.
- Idempotent: re-runs detect MARKER and do nothing.
- To revert to stock behavior, delete the hook block in main/CMakeLists.txt,
  delete this file, and run `idf.py fullclean` (managed_components/ is
  re-downloaded pristine).
"""
import hashlib
import pathlib
import sys

EPD_CPP = (
    pathlib.Path(__file__).resolve().parent.parent
    / "managed_components"
    / "m5stack__m5gfx"
    / "src"
    / "lgfx"
    / "v1"
    / "platforms"
    / "esp32"
    / "Panel_EPD.cpp"
)

# sha256 of the exact upstream Panel_EPD.cpp this patch applies to.
EXPECTED_PRISTINE = (
    "783699ae0eb2bca04cde9c5958ba3946e516833f95d7903da0e8db18270c16a4"
)

MARKER = "M5MangaS3Plus Kindle-style"

OLD_ERASER = """  static constexpr const uint32_t lut_eraser[] = {
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 1, 1),
    LUT_MAKE(2, 2, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    ~0u,
    0u,
  };"""

NEW_ERASER = """  // >>> M5MangaS3Plus Kindle-style eraser (tools/patch_epd_lut.py).
  // Uniform all-black flash (2 frames); end marker hands pixels to the
  // pending quality pointer. Indexed by OLD pixels but drives every level
  // to black identically. 3 scans instead of 4.
  static constexpr const uint32_t lut_eraser[] = {
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    0u,
  };"""

OLD_QUALITY = """  static constexpr const uint32_t lut_quality[] = {
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1, 1, 1, 1, 1),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 2, 2, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 2, 2, 2),
    LUT_MAKE(1, 1, 3, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2),
    LUT_MAKE(3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3),
    ~0u, ~0u, ~0u, ~0u,
    ~0u, ~0u, ~0u, ~0u,
    ~0u, ~0u, ~0u, ~0u,
    ~0u, ~0u, ~0u, ~0u,
    0u,
  };"""

NEW_QUALITY = """  // >>> M5MangaS3Plus Kindle-style quality waveform (tools/patch_epd_lut.py).
  // White flash (2) + stock vendor 16-gray image rows verbatim (10) + short
  // settle (2) + end. Runs after the all-black lut_eraser above, giving a
  // black -> white -> image sequence. 15 scans instead of 32.
  static constexpr const uint32_t lut_quality[] = {
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 2, 2, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 2, 2, 2),
    LUT_MAKE(1, 1, 3, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2),
    LUT_MAKE(3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3),
    ~0u, ~0u,
    0u,
  };"""


def main() -> int:
    if not EPD_CPP.is_file():
        print(f"EPD LUT patch: {EPD_CPP} not found "
              f"(managed_components not downloaded yet?)")
        return 1
    text = EPD_CPP.read_text()
    if MARKER in text:
        print("EPD LUT patch: already applied, skipping.")
        return 0
    digest = hashlib.sha256(text.encode()).hexdigest()
    if digest != EXPECTED_PRISTINE:
        print("EPD LUT patch: REFUSED - Panel_EPD.cpp hash mismatch.\n"
              f"  expected: {EXPECTED_PRISTINE}\n"
              f"  actual:   {digest}\n"
              "  M5GFX was probably updated; re-verify the LUT tables and "
              "update EXPECTED_PRISTINE in tools/patch_epd_lut.py.")
        return 1
    for name, old, new in (("lut_eraser", OLD_ERASER, NEW_ERASER),
                           ("lut_quality", OLD_QUALITY, NEW_QUALITY)):
        if text.count(old) != 1:
            print(f"EPD LUT patch: anchor block for {name} found "
                  f"{text.count(old)}x (expected exactly 1x), aborting.")
            return 1
        text = text.replace(old, new)
    EPD_CPP.write_text(text)
    print("EPD LUT patch: Kindle-style lut_eraser/lut_quality applied "
          "(~18 scans per quality refresh, was ~36).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

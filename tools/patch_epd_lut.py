#!/usr/bin/env python3
"""Kindle-style EPD quality waveform + stock one-shot for M5PaperS3.

Background: stock M5GFX drives every epd_quality refresh as ~36 full panel
scans. For fast page turns this project normally uses a short Kindle-like
script instead (black flash -> white -> image, ~18 scans). But the power-off
splash persists on screen unwatched, so it deserves one full original
refresh to settle ghost-free.

What this does to the managed M5GFX copy (Panel_EPD only, data + one
method; the step/retire engine is untouched):

  1. Kindle tables (default boot behavior): lut_eraser/lut_quality hold a
     short black -> white -> image script (~18 scans).
  2. Solid tables (on demand): lut_eraser_stock (long all-black saturate)
     + lut_quality_stock (long all-white saturate, then the vendor gray
     rows, then settle holds). The per-frame scan sweep is panel physics
     and can't be removed, but holding each uniform drive lets the pigment
     converge, so phases read as clean solids instead of moving bands.
  3. Panel_EPD::refreshStockWaveform(): waits for idle, parks every pixel
     at eraser step 0 (value kept as LUT index), and re-expands the LUT
     from the solid tables. The next display() then runs one full clean
     sequence ending on the new image. No switch-back: the only caller is
     the power-off splash; reboot re-expands the Kindle tables.

Tune SOLID_*_FRAMES below and rebuild to experiment: more frames = more
solid but slower; fewer = faster but the sweep may show again.

Usage from the app (see fullRefresh() in main/ui.cpp):
  draw splash into gSprite (no display yet) -> refreshStockWaveform() ->
  push + display() in epd_quality -> waitDisplay() -> power off.

Safety:
- Runs at CMake configure time via main/CMakeLists.txt (hook there).
- Pinned by SHA256 to the exact upstream files it was written for
  (M5GFX 0.2.29, see dependencies.lock). Unknown input is REFUSED, never
  half-patched. Re-verify anchors and update EXPECTED_* on M5GFX updates.
- Idempotent: re-runs detect MARKER_V2 in both files and do nothing.
- To revert fully, delete the hook block in main/CMakeLists.txt, delete
  this file, and run `idf.py fullclean` (managed_components/ is
  re-downloaded pristine).

Test hooks (not used by the build): argv [cpp_path] [hpp_path] override the
targets, and M5MANGA_EPD_EXPECTED_CPP / M5MANGA_EPD_EXPECTED_HPP override
the pristine hashes, so fixtures can exercise every state.
"""
import hashlib
import os
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def targets():
    if len(sys.argv) > 2:
        return pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    return (REPO_ROOT.joinpath(*PP), REPO_ROOT.joinpath(*HP))


PP = ("managed_components", "m5stack__m5gfx", "src", "lgfx", "v1",
      "platforms", "esp32", "Panel_EPD.cpp")
HP = ("managed_components", "m5stack__m5gfx", "src", "lgfx", "v1",
      "platforms", "esp32", "Panel_EPD.hpp")

# sha256 of the exact upstream files this patch applies to.
EXPECTED_PRISTINE_CPP = os.environ.get(
    "M5MANGA_EPD_EXPECTED_CPP",
    "783699ae0eb2bca04cde9c5958ba3946e516833f95d7903da0e8db18270c16a4")
EXPECTED_PRISTINE_HPP = os.environ.get(
    "M5MANGA_EPD_EXPECTED_HPP",
    "600c1ab594fb26cdab16c1b827a5b017c364c174d680f1322211f60543307df3")

MARKER_V1 = "M5MangaS3Plus Kindle-style"
MARKER_V2 = "M5MangaS3Plus stock waveform"

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

# --- Solid clean-refresh tuning (frames per phase) ---
# Each frame is one full panel scan (~70ms). Uniform drives converge after
# a few frames, so longer holds read as clean solids instead of sweeps.
SOLID_BLACK_FRAMES = 8
SOLID_WHITE_FRAMES = 8
SOLID_HOLD_FRAMES = 4

_BLACK_ROW = ("    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),")
_WHITE_ROW = ("    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),")
# Vendor-tuned 16-gray convergence rows (same as the Kindle table).
_IMAGE_ROWS = [
    "    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1),",
    "    LUT_MAKE(1, 1, 2, 2, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 3),",
    "    LUT_MAKE(1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 2, 2, 2),",
    "    LUT_MAKE(1, 1, 3, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2),",
    "    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),",
    "    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),",
    "    LUT_MAKE(1, 1, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),",
    "    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2),",
    "    LUT_MAKE(3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3),",
    "    LUT_MAKE(3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3),",
]


def _hold_lines():
    lines, left = [], SOLID_HOLD_FRAMES
    while left > 0:
        n = min(4, left)
        lines.append("    " + ", ".join(["~0u"] * n) + ",")
        left -= n
    return lines


def _build_stock_tables():
    lines = [
        "  // >>> M5MangaS3Plus stock waveform tables (tools/patch_epd_lut.py).",
        "  // Solid clean-refresh copies for Panel_EPD::refreshStockWaveform():",
        "  // long all-black saturate, long all-white saturate, vendor gray",
        "  // rows, then settle holds. Frame counts from SOLID_*_FRAMES.",
        "  static constexpr const uint32_t lut_eraser_stock[] = {",
        *([_BLACK_ROW] * SOLID_BLACK_FRAMES),
        "    0u,",
        "  };",
        "  static constexpr const size_t lut_eraser_stock_step =",
        "      sizeof(lut_eraser_stock) / sizeof(uint32_t);",
        "  static constexpr const uint32_t lut_quality_stock[] = {",
        *([_WHITE_ROW] * SOLID_WHITE_FRAMES),
        *_IMAGE_ROWS,
        *_hold_lines(),
        "    0u,",
        "  };",
    ]
    return "\n".join(lines) + "\n"


STOCK_TABLES = _build_stock_tables()

# Matches a previously injected stock-tables block (whatever its row counts)
# for migration to the current generated text.
OLD_STOCK_RE = re.compile(
    r"  // >>> M5MangaS3Plus stock waveform tables.*?"
    r"lut_quality_stock\[\] = \{\n.*?\n  \};\n",
    re.DOTALL)

ENGINE_METHOD = """  // >>> M5MangaS3Plus stock waveform one-shot (tools/patch_epd_lut.py).
  // Rebuilds the expanded LUT from the STOCK tables and parks every pixel
  // at eraser step 0 (value kept as LUT index), so the next display() runs
  // one full original-quality sequence (~36 scans) ending on the new image.
  // No switch-back: the only caller is the power-off splash, and reboot
  // re-expands the Kindle tables from scratch.
  void Panel_EPD::refreshStockWaveform(void)
  {
    waitDisplay();
    if (!_step_framebuf || !_lut_2pixel) return;
    const size_t cells =
        (size_t)_cfg.memory_width * (size_t)_cfg.memory_height / 2;
    for (size_t i = 0; i < cells; ++i) {
      _step_framebuf[i * 2] &= 0xFF;
      _step_framebuf[i * 2 + 1] ^= 0xFFFF;
    }
    const size_t total = lut_eraser_stock_step
                       + sizeof(lut_quality_stock) / sizeof(uint32_t)
                       + _config_detail.lut_text_step
                       + _config_detail.lut_fast_step
                       + _config_detail.lut_fastest_step;
    auto *buf = (uint8_t *)heap_caps_malloc(total * 256 * sizeof(uint16_t),
                                            MALLOC_CAP_DMA);
    if (!buf) return;  // keep Kindle tables; splash still shows
    memset(buf, 0x0F, 256);
    size_t lindex = 0;
    for (int epd_mode = 0; epd_mode < 5; ++epd_mode) {
      const uint32_t* lut_src = nullptr;
      size_t lut_step = 0;
      switch (epd_mode) {
        default:                      lut_src = lut_eraser_stock; lut_step = lut_eraser_stock_step; break;
        case epd_mode_t::epd_quality: lut_src = lut_quality_stock; lut_step = sizeof(lut_quality_stock) / sizeof(uint32_t); break;
        case epd_mode_t::epd_text:    lut_src = _config_detail.lut_text;    lut_step = _config_detail.lut_text_step;    break;
        case epd_mode_t::epd_fast:    lut_src = _config_detail.lut_fast;    lut_step = _config_detail.lut_fast_step;    break;
        case epd_mode_t::epd_fastest: lut_src = _config_detail.lut_fastest; lut_step = _config_detail.lut_fastest_step; break;
      }
      if (lut_src == nullptr) { continue; }
      _lut_offset_table[epd_mode] = lindex >> 8;
      _lut_remain_table[epd_mode] = lut_step;
      for (int step = 0; step < lut_step; ++step) {
        auto lu = lut_src[0];
        for (int lv = 0; lv < 256; ++lv) {
          buf[lindex] = (((lu >> ((lv >> 4) << 1)) & 3) << 2) + ((lu >> ((lv & 15) << 1)) & 3);
          ++lindex;
        }
        ++lut_src;
      }
    }
    heap_caps_free(_lut_2pixel);
    _lut_2pixel = buf;
  }

"""

HPP_DECL = """    // M5MangaS3Plus stock waveform one-shot (tools/patch_epd_lut.py).
    void refreshStockWaveform(void);
"""


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        print(f"EPD patch: anchor for {what} found {n}x "
              f"(expected exactly 1x), aborting.")
        return None
    return text.replace(old, new)


def main() -> int:
    EPD_CPP, EPD_HPP = targets()
    if not EPD_CPP.is_file() or not EPD_HPP.is_file():
        print("EPD patch: Panel_EPD sources not found "
              "(managed_components not downloaded yet?)")
        return 1
    cpp = EPD_CPP.read_text()
    hpp = EPD_HPP.read_text()

    has_solid = "Solid clean-refresh copies" in cpp
    has_decl = MARKER_V2 in hpp
    if has_solid and has_decl:
        print("EPD patch: already applied, skipping.")
        return 0

    if MARKER_V1 not in cpp:
        if sha256(EPD_CPP) != EXPECTED_PRISTINE_CPP:
            print("EPD patch: REFUSED - Panel_EPD.cpp hash mismatch.\n"
                  "  M5GFX was probably updated; re-verify anchors and "
                  "update EXPECTED_PRISTINE_CPP in tools/patch_epd_lut.py.")
            return 1
        for name, old, new in (("lut_eraser", OLD_ERASER, NEW_ERASER),
                               ("lut_quality", OLD_QUALITY, NEW_QUALITY)):
            cpp = replace_once(cpp, old, new, name)
            if cpp is None:
                return 1

    # Stock tables: migrate an older injected block, or insert fresh.
    if not has_solid:
        m = OLD_STOCK_RE.search(cpp)
        if m:
            cpp = cpp[:m.start()] + STOCK_TABLES + cpp[m.end():]
        elif "lut_eraser_stock" not in cpp:
            r = replace_once(cpp, "#undef LUT_MAKE",
                             STOCK_TABLES + "#undef LUT_MAKE", "stock tables")
            if r is None:
                return 1
            cpp = r
        else:
            print("EPD patch: unrecognized stock-tables state, aborting.\n"
                  "  Run `idf.py fullclean` and rebuild.")
            return 1

    if "void Panel_EPD::refreshStockWaveform(void)" not in cpp:
        r = replace_once(
            cpp, "  void Panel_EPD::beginTransaction(void)",
            ENGINE_METHOD + "  void Panel_EPD::beginTransaction(void)",
            "engine method")
        if r is None:
            return 1
        cpp = r

    if not has_decl:
        if sha256(EPD_HPP) != EXPECTED_PRISTINE_HPP:
            print("EPD patch: REFUSED - Panel_EPD.hpp hash mismatch.\n"
                  "  M5GFX was probably updated; re-verify anchors and "
                  "update EXPECTED_PRISTINE_HPP in tools/patch_epd_lut.py.")
            return 1
        r = replace_once(hpp, "    void setPowerSave(bool flg) override;",
                         "    void setPowerSave(bool flg) override;\n" +
                         HPP_DECL, "hpp decl")
        if r is None:
            return 1
        hpp = r

    EPD_CPP.write_text(cpp)
    EPD_HPP.write_text(hpp)
    print("EPD patch: Kindle tables + solid one-shot applied "
          "(default ~18 scans; fullRefresh() runs the solid ~32).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

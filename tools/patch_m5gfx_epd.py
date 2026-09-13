#!/usr/bin/env python3
"""Restore the M5GFX 0.2.7 e-paper engine for Arduino-grade refresh speed.

Background: M5GFX >= 0.2.28 rewrote the PaperS3 EPD engine (dual-slot phase
machine + separate eraser section) and extended the `epd_quality` waveform
from 20 to 31 panel phases, while the scan engine, DMA clock and bus are
unchanged. Result: quality-mode page turns need ~55% more frame passes than
with the Arduino build's M5GFX 0.2.7 (menus use `epd_fast`, which got
slightly faster - hence "snappy UI, slow page turns").

What this does: overwrites Panel_EPD.cpp/.hpp in the managed M5GFX copy
with the snapshots in tools/epd027/ (0.2.7 engine + waveforms, plus the
0.2.28 ESP32-S3 cache-line fix which 0.2.7 lacks). The IDF6-compatible bus
layer (Bus_EPD) stays at the current version.

Safety:
- Runs at CMake configure time via main/CMakeLists.txt (see hook there).
- Pinned by SHA256 to the exact upstream files it was written for. Any M5GFX
  update changes the hashes -> configure stops with a loud error instead of
  silently building a mismatched tree. Re-verify and update EXPECTED_* then.
- Idempotent: re-runs detect the snapshot MARKER and do nothing.
- To revert to stock upstream behavior, delete the hook block in
  main/CMakeLists.txt and this file plus tools/epd027/, then rebuild.
  (Note: managed_components/ is re-downloaded pristine by fullclean.)
"""
import hashlib
import pathlib
import shutil
import sys

EPD_DIR = (
    pathlib.Path(__file__).resolve().parent.parent
    / "managed_components"
    / "m5stack__m5gfx"
    / "src"
    / "lgfx"
    / "v1"
    / "platforms"
    / "esp32"
)
SNAP_DIR = pathlib.Path(__file__).resolve().parent / "epd027"

# sha256 of the exact upstream files this patch applies to (M5GFX 0.2.28).
EXPECTED = {
    "Panel_EPD.cpp": (
        "9bbf1405ebe3bf059337099b2a195476bf275b15613ea4ae7b90c604af4c5d26"
    ),
    "Panel_EPD.hpp": (
        "600c1ab594fb26cdab16c1b827a5b017c364c174d680f1322211f60543307df3"
    ),
}

MARKER = "M5MangaS3Plus NOTE"


def main() -> int:
    for name, want in EXPECTED.items():
        target = EPD_DIR / name
        snap = SNAP_DIR / name
        if not target.exists():
            print(f"FATAL: {target} not found. Run idf.py build once so the "
                  f"component manager downloads M5GFX, then rebuild.")
            return 1
        if not snap.exists():
            print(f"FATAL: snapshot {snap} missing from the repo.")
            return 1
        text = target.read_text(encoding="utf-8")
        if MARKER in text:
            print(f"{name} already patched, skipping.")
            continue
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        if digest != want:
            print(f"FATAL: {name} hash mismatch.\n"
                  f"  expected: {want}\n"
                  f"  actual:   {digest}\n"
                  f"The vendored M5GFX version changed - re-verify the EPD "
                  f"engine snapshot and update tools/patch_m5gfx_epd.py.")
            return 1
        shutil.copyfile(snap, target)
        print(f"{name} reverted to 0.2.7 EPD engine snapshot.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

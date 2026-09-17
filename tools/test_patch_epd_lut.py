#!/usr/bin/env python3
"""Regression tests for patch_epd_lut.py (fixtures only, stdlib only).

Covers every branch of the patch state machine: pristine -> full patch,
v1 (Kindle-only) -> delta, idempotent re-runs, and all refusal paths.
Run: python3 tools/test_patch_epd_lut.py
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
import patch_epd_lut as P

FAKE_CPP_PRISTINE = (
    "// fake Panel_EPD.cpp\n" + P.OLD_ERASER + "\n" + P.OLD_QUALITY + "\n" +
    "#undef LUT_MAKE\n"
    "  static constexpr const size_t lut_eraser_step = "
    "sizeof(lut_eraser) / sizeof(uint32_t);\n"
    "  void Panel_EPD::beginTransaction(void)\n  {\n  }\n")

FAKE_CPP_V1 = (FAKE_CPP_PRISTINE.replace(P.OLD_ERASER, P.NEW_ERASER).replace(
    P.OLD_QUALITY, P.NEW_QUALITY))

FAKE_HPP = ("// fake Panel_EPD.hpp\n"
            "    void setPowerSave(bool flg) override;\n")


def run_patch(cpp_text, hpp_text, cpp_hash=None, hpp_hash=None):
    """Run the script against fixture files. Returns (rc, output, cpp, hpp).
    None text means the file is not created (missing-file case)."""
    tmp = tempfile.mkdtemp()
    cpp = os.path.join(tmp, "Panel_EPD.cpp")
    hpp = os.path.join(tmp, "Panel_EPD.hpp")
    if cpp_text is not None:
        with open(cpp, "w") as f:
            f.write(cpp_text)
    if hpp_text is not None:
        with open(hpp, "w") as f:
            f.write(hpp_text)
    env = dict(os.environ)
    if cpp_hash is None and cpp_text is not None:
        cpp_hash = hashlib.sha256(cpp_text.encode()).hexdigest()
    if hpp_hash is None and hpp_text is not None:
        hpp_hash = hashlib.sha256(hpp_text.encode()).hexdigest()
    if cpp_hash is not None:
        env["M5MANGA_EPD_EXPECTED_CPP"] = cpp_hash
    if hpp_hash is not None:
        env["M5MANGA_EPD_EXPECTED_HPP"] = hpp_hash
    r = subprocess.run(
        [sys.executable,
         os.path.join(os.path.dirname(__file__), "patch_epd_lut.py"), cpp,
         hpp],
        capture_output=True,
        text=True,
        env=env)
    def read(p):
        if not os.path.exists(p):
            return None
        with open(p) as f:
            return f.read()
    return r.returncode, r.stdout + r.stderr, read(cpp), read(hpp)


class PatchTests(unittest.TestCase):
    def test_pristine_full_patch(self):
        rc, out, cpp, hpp = run_patch(FAKE_CPP_PRISTINE, FAKE_HPP)
        self.assertEqual(rc, 0, out)
        self.assertIn("lut_eraser_stock", cpp)
        self.assertIn("lut_quality_stock", cpp)
        self.assertIn("refreshStockWaveform", cpp)
        self.assertIn("refreshStockWaveform", hpp)
        self.assertIn("LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1)",
                      cpp)  # Kindle eraser applied too
        self.assertNotIn(P.OLD_ERASER, cpp)
        black = ("LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),")
        white = ("LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),")
        self.assertEqual(cpp.count(black), 2 + P.SOLID_BLACK_FRAMES)
        self.assertEqual(cpp.count(white), 2 + P.SOLID_WHITE_FRAMES)

    def test_idempotent_rerun(self):
        rc1, _, cpp1, hpp1 = run_patch(FAKE_CPP_PRISTINE, FAKE_HPP)
        self.assertEqual(rc1, 0)
        # Feed outputs back as inputs with their new hashes.
        rc, out, cpp2, hpp2 = run_patch(cpp1, hpp1)
        self.assertEqual(rc, 0, out)
        self.assertIn("already applied", out)
        self.assertEqual(cpp1, cpp2)
        self.assertEqual(hpp1, hpp2)

    def test_v1_delta(self):
        rc, out, cpp, hpp = run_patch(FAKE_CPP_V1, FAKE_HPP)
        self.assertEqual(rc, 0, out)
        self.assertIn("refreshStockWaveform", cpp)
        self.assertIn("refreshStockWaveform", hpp)
        # Kindle blocks must not be duplicated.
        self.assertEqual(cpp.count("Kindle-style eraser"), 1)
        self.assertEqual(cpp.count("Kindle-style quality"), 1)

    def test_corrupt_refused(self):
        rc, out, cpp, hpp = run_patch("garbage\n", FAKE_HPP,
                                      cpp_hash="0" * 64)
        self.assertEqual(rc, 1)
        self.assertIn("REFUSED", out)
        self.assertEqual(cpp, "garbage\n")  # untouched

    def test_partial_state_completes(self):
        # cpp already migrated, hpp untouched: the run finishes the pair.
        rc1, _, cpp_v2, _ = run_patch(FAKE_CPP_PRISTINE, FAKE_HPP)
        self.assertEqual(rc1, 0)
        rc, out, cpp, hpp = run_patch(cpp_v2, FAKE_HPP)
        self.assertEqual(rc, 0, out)
        self.assertIn("refreshStockWaveform", hpp)

    def test_old_stock_migrates(self):
        # Previous script version's tables are replaced by solid counts.
        old_block = ("  // >>> M5MangaS3Plus stock waveform tables X.\n"
                     "  // Pristine upstream copies for Y.\n"
                     "  static constexpr const uint32_t lut_eraser_stock[] = {\n"
                     "    0u,\n"
                     "  };\n"
                     "  static constexpr const uint32_t lut_quality_stock[] = {\n"
                     "    0u,\n"
                     "  };\n")
        v1cpp = FAKE_CPP_V1.replace("#undef LUT_MAKE",
                                    old_block + "#undef LUT_MAKE")
        rc, out, cpp, _ = run_patch(v1cpp, FAKE_HPP)
        self.assertEqual(rc, 0, out)
        self.assertIn("Solid clean-refresh copies", cpp)
        self.assertNotIn("Pristine upstream copies", cpp)
        self.assertEqual(cpp.count("lut_eraser_stock[] = {"), 1)

    def test_hpp_mismatch_refused(self):
        rc, out, _, _ = run_patch(FAKE_CPP_PRISTINE, "other\n",
                                  hpp_hash="0" * 64)
        self.assertEqual(rc, 1)
        self.assertIn("REFUSED", out)

    def test_missing_files(self):
        rc, out, _, _ = run_patch(None, None)
        self.assertEqual(rc, 1)
        self.assertIn("not found", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)

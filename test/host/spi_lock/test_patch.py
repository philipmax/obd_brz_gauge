#!/usr/bin/env python3
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/idf_compat"))
from patch_spi_bus_lock import patched_source

SOURCE = Path(sys.argv.pop(1)).read_text()
PATCH = (ROOT / "tools/idf_compat/spi_bus_lock.patch").read_text()


class PatchTests(unittest.TestCase):
    def test_real_sdk_and_idempotence(self):
        fixed = patched_source(SOURCE, PATCH)
        self.assertNotEqual(SOURCE, fixed)
        self.assertEqual(fixed, patched_source(fixed, PATCH))

    def test_unknown_context_rejected(self):
        with self.assertRaises(ValueError):
            patched_source(SOURCE.replace('bool bg_yield;', 'bool renamed;'), PATCH)

    def test_duplicate_context_rejected(self):
        with self.assertRaises(ValueError):
            patched_source(SOURCE + SOURCE, PATCH)

    def test_empty_patch_rejected(self):
        with self.assertRaises(ValueError):
            patched_source(SOURCE, '')


if __name__ == '__main__':
    unittest.main()

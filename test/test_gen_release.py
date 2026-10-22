"""Release generation must validate before changing the public package."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("gen_release", Path(__file__).resolve().parents[1] / "tools/gen_release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseGenerationTest(unittest.TestCase):
    def setUp(self):
        self.area = tempfile.TemporaryDirectory()
        self.addCleanup(self.area.cleanup)
        self.root = Path(self.area.name)
        self.build = self.root / "build"
        self.output = self.root / "release"
        self.output.mkdir()
        (self.output / "latest.json").write_text("original")
        self.tag = b"rebuild-42-123456789abc\0"
        for key, rel in release.BINS.items():
            if key == "bootmedia":
                continue
            path = self.build / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.tag if key == "firmware" else b"fixture")
        self.addCleanup(patch.stopall)
        patch.object(release, "BUILD_DIR", str(self.build)).start()
        patch.object(release, "RELEASE_DIR", str(self.output)).start()
        patch.object(release, "git", side_effect=lambda *args: {
            ("rev-parse", "--abbrev-ref", "HEAD"): "rebuild",
            ("rev-list", "--count", "HEAD"): "42",
            ("rev-parse", "--short=12", "HEAD"): "123456789abc",
        }[args]).start()

    def test_sd_only_release_omits_legacy_media_and_removes_stale_copy(self):
        (self.output / "bootmedia.bin").write_bytes(b"old")
        release.main()
        manifest = json.loads((self.output / "latest.json").read_text())
        self.assertNotIn("bootmedia", manifest["files"])
        self.assertEqual({}, manifest["sd_files"])
        self.assertFalse((self.output / "bootmedia.bin").exists())
        self.assertEqual(4, len(manifest["files"]))
        self.assertEqual(self.tag, (self.output / "obd_brz_gauge.bin").read_bytes())

    def test_invalid_tag_or_missing_file_does_not_modify_release(self):
        firmware = self.build / "obd_brz_gauge.bin"
        firmware.write_bytes(self.tag[:-1] + b"extra\0")
        with self.assertRaises(SystemExit):
            release.main()
        self.assertEqual("original", (self.output / "latest.json").read_text())
        self.assertFalse((self.output / "obd_brz_gauge.bin").exists())
        firmware.unlink()
        with self.assertRaises(SystemExit):
            release.main()
        self.assertEqual("original", (self.output / "latest.json").read_text())

    def test_optional_legacy_raw_media_is_included_when_valid(self):
        media = self.build / "bootmedia.bin"
        media.write_bytes(b"format=1\n" + b"\xff" * 4096)
        release.main()
        manifest = json.loads((self.output / "latest.json").read_text())
        self.assertEqual(media.stat().st_size, manifest["files"]["bootmedia"]["size"])
        self.assertEqual({"/OBD/BOOT/bootmedia.raw": "bootmedia.bin"}, manifest["sd_files"])


if __name__ == "__main__":
    unittest.main()

"""Pressure-domain regressions for the distributed turbo profiles; no IDF build."""
import importlib.util
import json
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SPEC = importlib.util.spec_from_file_location("expr2rpn", HERE / "expr2rpn.py")
expr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(expr)


class BoostProfilesTest(unittest.TestCase):
    def test_boost_pid_and_map_fallback(self):
        checked = 0
        for directory in [ROOT / "vehicles/catalog", ROOT / "vehicles/import/OBD/IMPORT"]:
            for path in directory.glob("*.vehicle.json"):
                profile = json.loads(path.read_text())
                for signal in profile.get("signals", []):
                    if signal.get("id") != "obd.boost":
                        continue
                    checked += 1
                    with self.subTest(profile=profile["id"]):
                        self.assertEqual(signal["unit"], "kPa_rel")
                        self.assertGreaterEqual(profile["rev"], 2)
                        lo, hi = signal["post"]["clamp"]
                        if profile["id"] in ("obd2.generic", "custom.obd2.generic"):
                            primary, fallback = signal["src"]
                            self.assertEqual(primary["req"], "0170")
                            self.assertEqual(primary["len"], 10)
                            self.assertTrue(primary["multiframe"])
                            self.assertEqual(primary["multiframe_mode"], "elm")
                            self.assertEqual(fallback["req"], "010B")
                            # PID 70 carries absolute kPa in bytes D/E at 1/32 kPa.
                            for absolute_kpa, relative_kpa in [(35, -65), (100, 0), (180, 80), (255, 155)]:
                                raw = absolute_kpa * 32
                                data = [0, 0, 0, raw >> 8, raw & 0xFF, 0, 0, 0, 0, 0]
                                value = expr.evaluate(primary["rpn"], data)
                                self.assertEqual(max(lo, min(hi, value)), relative_kpa)
                            # The unsupported/zero sensor result is rejected, allowing MAP fallback.
                            zero_result = expr.evaluate(primary["rpn"], [0] * 10)
                            self.assertLess(zero_result, primary["valid"][0])
                        else:
                            source = signal["src"][0]
                            self.assertEqual(source["req"], "010B")
                            # Fixed 100 kPa reference used by legacy profiles.
                            for absolute_kpa, relative_kpa in [(0, -100), (35, -65), (100, 0), (180, 80), (255, 155)]:
                                value = expr.evaluate(source["rpn"], [absolute_kpa])
                                self.assertEqual(max(lo, min(hi, value)), relative_kpa)
        self.assertEqual(checked, 17)  # 8 catalog profiles + 9 editable templates


if __name__ == "__main__":
    unittest.main()

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
    def test_manifold_vacuum_zero_and_positive_pressure(self):
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
                        source = signal["src"][0]
                        self.assertEqual(source["req"], "010B")
                        lo, hi = signal["post"]["clamp"]
                        # Fixed 100 kPa reference used by these profiles:
                        # -1.00 / -0.65 / 0.00 / +0.80 / +1.55 bar.
                        for absolute_kpa, relative_kpa in [(0, -100), (35, -65), (100, 0), (180, 80), (255, 155)]:
                            value = expr.evaluate(source["rpn"], [absolute_kpa])
                            self.assertEqual(max(lo, min(hi, value)), relative_kpa)
        self.assertEqual(checked, 17)  # 8 catalog profiles + 9 editable templates


if __name__ == "__main__":
    unittest.main()

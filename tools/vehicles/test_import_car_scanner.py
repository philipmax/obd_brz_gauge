import importlib.util
import os
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "import_car_scanner.py")
SPEC = importlib.util.spec_from_file_location("import_car_scanner", SCRIPT)
converter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(converter)


class VehicleYearsTest(unittest.TestCase):
    def test_closed_year_range(self):
        self.assertEqual(
            converter.vehicle_years({"Name": "BRZ (gen.1 2012-2021)"}),
            (2012, 2021),
        )

    def test_open_ended_ranges(self):
        cases = {
            "500e (2020-)": (2020, None),
            "300 EXC Continental 2023->": (2023, None),
            "C11 (2023-present)": (2023, None),
            "2010 - n.d.": (2010, None),
        }
        for name, years in cases.items():
            with self.subTest(name=name):
                self.assertEqual(converter.vehicle_years({"Name": name}),
                                 years)

    def test_no_year_metadata(self):
        self.assertEqual(converter.vehicle_years({"Name": "OBD-II / EOBD"}),
                         (None, None))


class CarScannerFormulaTest(unittest.TestCase):
    def test_arithmetic_and_extended_byte_names(self):
        rpn, length = converter.car_scanner_expression("(A*256+B)/4 + AX")
        self.assertEqual(length, 50)
        self.assertEqual(rpn[0], ["A", 0])
        self.assertIn(["A", 49], rpn)

    def test_bitwise_functions_and_signed_byte(self):
        rpn, length = converter.car_scanner_expression(
            "And(A, 7) + SIGNED(B) + Shr(C, 1)")
        self.assertEqual(length, 3)
        self.assertIn("&", rpn)
        self.assertIn("sext8", rpn)
        self.assertIn(">>", rpn)

    def test_conditional_and_float32_are_rejected(self):
        for formula in ("if(A=1;A;B)", "FLOAT32(A,B,C,D)"):
            with self.subTest(formula=formula):
                with self.assertRaises(ValueError):
                    converter.car_scanner_expression(formula)

    def test_reverse_byte_order_maps_to_little_endian(self):
        signal, reason = converter.convert_pid({
            "CMD": "22F190",
            "SBI": 0,
            "DL": 2,
            "MUL": 1,
            "DIV": 1,
            "OFS": 0,
            "RBS": True,
            "SNM": "Value",
        })
        self.assertIsNone(reason)
        self.assertEqual(signal["src"][0]["rpn"], [["R", 0, 16, "le"]])

    def test_external_scaling_with_formula_is_not_guessed(self):
        signal, reason = converter.convert_pid({
            "CMD": "22F190",
            "SBI": 0,
            "DL": 2,
            "MUL": 2,
            "DIV": 1,
            "OFS": 0,
            "FR": "A+B",
            "SNM": "Value",
        })
        self.assertIsNone(signal)
        self.assertEqual(reason, "formula_with_external_scaling")


if __name__ == "__main__":
    unittest.main()

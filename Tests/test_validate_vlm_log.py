import unittest
from validate_vlm_log import validate_log


class ValidationTests(unittest.TestCase):
    GOOD = """vlm validate quantization: maxRelErr=3.8e-4
vlm validate const-sky: maxRelErr=0.007588
vlm probe 0 E(+Y)=3.14,1.57,0.785
vlm bake: wrote test.vlm (1 probes, 189 bytes)
vlm: runtime activated (1 probes)
"""

    def test_valid(self):
        self.assertAlmostEqual(validate_log(self.GOOD, "const")["constSky"], .007588)

    def test_fail_closed(self):
        for text in (self.GOOD.replace("0.007588", "nan"),
                     self.GOOD.replace("0.007588", "0.02"),
                     self.GOOD.replace("3.14,", "inf,"),
                     self.GOOD.replace("vlm: runtime activated", "skipped"),
                     self.GOOD + "\nValidation Error: VUID-test",
                     self.GOOD.replace("vlm validate const-sky:", "skipped:")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                validate_log(text, "const")


if __name__ == "__main__":
    unittest.main()

"""Exercise ordinary encoder math and the real driver against fake peripherals."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


class OrdinaryNativeTests(unittest.TestCase):
    def compile_and_run(self, name, extra_sources=(), arguments=((),)):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / (name + (".exe" if os.name == "nt" else ""))
            command = [os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-I", str(REPO / "main"), "-I", str(TESTS / "stubs"),
                       str(TESTS / (name + ".cpp")), *map(str, extra_sources), "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for args in arguments:
                executed = subprocess.run([str(binary), *args], capture_output=True, text=True)
                self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_math_and_low_speed_estimator(self):
        self.compile_and_run("test_encoder_math")

    def test_actual_acquisition_independent_channels(self):
        self.compile_and_run("test_acquisition", [REPO / "ordinary_caster" / "encoders.cpp"])

    def test_split_boards_never_initialize_or_read_other_sensor(self):
        self.compile_and_run("test_split_acquisition", [REPO / "ordinary_caster" / "encoders.cpp"],
                             (("5",), ("5", "unverified"), ("6",)))


if __name__ == "__main__":
    unittest.main()

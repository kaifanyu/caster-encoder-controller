"""Run real recorder.cpp against deterministic SDK/flash substitutes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
CJSON = os.environ.get("CJSON_SOURCE_DIRECTORY")


@unittest.skipUnless(CJSON, "set CJSON_SOURCE_DIRECTORY to pinned ESP-IDF cJSON for recorder lifecycle tests")
class RecorderStorageTests(unittest.TestCase):
    def test_firmware_state_machine_and_torn_flash_recovery(self):
        self._run_profile(1, REPO / "config")

    def test_ordinary_dual_channel_storage_and_recovery(self):
        self._run_profile(4, REPO / "ordinary_caster" / "config")

    def test_separate_swivel_board_storage_and_recovery(self):
        self._run_profile(5, REPO / "ordinary_caster" / "config")

    def test_separate_wheel_board_storage_and_recovery(self):
        self._run_profile(6, REPO / "ordinary_caster" / "config")

    def _run_profile(self, node, config):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cjson = Path(CJSON)
            object_file = root / "cJSON.o"
            subprocess.run(["gcc", "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(object_file)],
                           check=True, capture_output=True, text=True)
            binary = root / ("recorder.exe" if os.name == "nt" else "recorder")
            command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-I", str(REPO / "tests" / "recorder_stubs"), "-I", str(REPO / "main"),
                       "-I", str(config), "-I", str(cjson),
                       f"-DCASTER_NODE_ID={node}", "-DCASTER_WHEEL_INDEX=1",
                       '-DCASTER_CONFIG_HEADER="front_left.example.h"',
                       str(REPO / "tests" / "test_recorder_storage.cpp"), str(object_file), "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertIn("torn writes", executed.stdout)


if __name__ == "__main__":
    unittest.main()

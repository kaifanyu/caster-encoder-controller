import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class ProfileTests(unittest.TestCase):
    def test_helper_passes_six_distinct_profile_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "scripts").mkdir()
            (root / "config").mkdir()
            (root / "bin").mkdir()
            shutil.copy2(REPO / "scripts" / "idf.sh", root / "scripts" / "idf.sh")
            fake = root / "bin" / "idf.py"
            fake.write_text('#!/usr/bin/env python3\nimport json, sys\n'
                            'print("ESP-IDF v5.4.2" if "--version" in sys.argv else json.dumps(sys.argv[1:]))\n')
            fake.chmod(0o755)
            env = {**os.environ, "PATH": str(root / "bin") + os.pathsep + os.environ["PATH"]}
            helper = str(root / "scripts" / "idf.sh")
            for wheel in ("front_left", "front_right"):
                config = root / "config" / f"{wheel}.local.h"
                missing = subprocess.run([helper, wheel, "H1", "build"], env=env, capture_output=True, text=True)
                self.assertEqual(missing.returncode, 2)
                self.assertIn(f"config/{wheel}.local.h", missing.stderr)
                config.write_text("// test\n")
                for role in ("H1", "H2", "CENTER"):
                    result = subprocess.run([helper, wheel, role, "build"], env=env,
                                            capture_output=True, text=True, check=True)
                    args = json.loads(result.stdout)
                    self.assertEqual(args[:2], ["-B", f"build/{wheel}/{role}"])
                    self.assertIn(f"-DSDKCONFIG={root}/build/{wheel}/{role}/sdkconfig", args)
                    self.assertIn(f"-DCASTER_WHEEL={wheel}", args)
                    self.assertIn(f"-DCASTER_ROLE={role}", args)
                    self.assertEqual(args[-1], "build")
            for args in (("left", "H1"), ("front_left", "H3"), ("front_left",),
                         ("front_left", "H1", "-DCASTER_WHEEL=front_right"),
                         ("front_left", "H1", "-B", "build/wrong"),
                         ("front_left", "H1", "-D", "CASTER_ROLE=H2"),
                         ("front_left", "H1", "--define-cache-entry=CASTER_WHEEL=front_right")):
                result = subprocess.run([helper, *args], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, args)

    def test_wheel_configurations_are_independent_for_all_roles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copy2(REPO / "config" / "example.h", root / "example.h")
            (root / "front_left.local.h").write_text(
                '#define UDP_PORT 5005\n#define H1_SIGN -1\n#define H2_SIGN 1\n'
                '#define CENTER_SIGN -1\n#define H1_PPR 1024\n#define H2_PPR 512\n'
                '#define H1_IPV4 "192.0.2.11"\n#define H2_IPV4 "192.0.2.12"\n'
                '#define CENTER_IPV4 "192.0.2.13"\n#include "example.h"\n')
            (root / "front_right.local.h").write_text(
                '#define UDP_PORT 5006\n#define H1_SIGN 1\n#define H2_SIGN -1\n'
                '#define CENTER_SIGN 1\n#define H1_PPR 2048\n#define H2_PPR 4096\n'
                '#define H1_IPV4 "192.0.2.21"\n#define H2_IPV4 "192.0.2.22"\n'
                '#define CENTER_IPV4 "192.0.2.23"\n#include "example.h"\n')
            source = root / "probe.cpp"
            source.write_text('#include "config.h"\n#include <iostream>\nint main() { '
                              'auto c = caster::configuration(); '
                              'std::cout << caster::kWheel << " " << caster::kRole << " " << UDP_PORT '
                              '<< " " << c.sign << " " << c.ppr << " " << caster::kLocalIpv4; }\n')
            for index, wheel in enumerate(("front_left", "front_right"), 1):
                for node, role in enumerate(("H1", "H2", "CENTER"), 1):
                    binary = root / f"{wheel}-{role}"
                    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(REPO / "main"), "-I", str(root),
                                    f"-DCASTER_NODE_ID={node}", f"-DCASTER_WHEEL_INDEX={index}",
                                    f'-DCASTER_CONFIG_HEADER="{wheel}.local.h"', str(source), "-o", str(binary)],
                                   check=True, capture_output=True, text=True)
                    result = subprocess.check_output([str(binary)], text=True).split()
                    self.assertEqual(result[:3], [wheel, role, str(5004 + index)])
                    expected_signs = (-1, 1, -1) if index == 1 else (1, -1, 1)
                    expected_ppr = (1024, 512, 512) if index == 1 else (2048, 4096, 4096)
                    self.assertEqual(int(result[3]), expected_signs[node - 1])
                    self.assertEqual(int(result[4]), expected_ppr[node - 1])
                    self.assertEqual(result[5], f"192.0.2.{index}{node}")


if __name__ == "__main__":
    unittest.main()

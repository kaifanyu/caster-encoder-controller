import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class ProfileTests(unittest.TestCase):
    def test_ball_family_config_and_legacy_fallback_reject_ambiguity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("scripts", "config", "bin", "ball_caster/config"):
                (root / name).mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / "scripts/idf.sh", root / "scripts/idf.sh")
            (root / "scripts/idf.sh").chmod(0o755)
            shutil.copy2(REPO / "CMakeLists.txt", root / "CMakeLists.txt")
            fake = root / "bin/idf.py"
            fake.write_text('#!/usr/bin/env python3\nimport json, sys\n'
                            'print("ESP-IDF v5.4.2" if "--version" in sys.argv else json.dumps(sys.argv[1:]))\n')
            fake.chmod(0o755)
            env = {**os.environ, "PATH": str(root / "bin") + os.pathsep + os.environ["PATH"]}
            helper = str(root / "scripts/idf.sh")
            for wheel in ("front_left", "front_right"):
                canonical = root / "ball_caster/config" / f"{wheel}.local.h"
                legacy = root / "config" / f"{wheel}.local.h"
                canonical.write_text("// canonical ball calibration\n")
                result = subprocess.run([helper, wheel, "CENTER", "build"], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                legacy.write_text("// legacy calibration must not be shadowed\n")
                rejected = subprocess.run([helper, wheel, "CENTER", "build"], env=env,
                                          capture_output=True, text=True)
                self.assertEqual(rejected.returncode, 2)
                self.assertIn("Ambiguous ball calibration", rejected.stderr)
                if shutil.which("cmake"):
                    rejected_cmake = subprocess.run(
                        ["cmake", "-S", str(root), "-B", str(root / f"cmake-{wheel}"),
                         f"-DCASTER_WHEEL={wheel}", "-DCASTER_ROLE=CENTER"],
                        capture_output=True, text=True)
                    self.assertNotEqual(rejected_cmake.returncode, 0)
                    self.assertIn("Ambiguous ball calibration", rejected_cmake.stderr)
                canonical.unlink()
                result = subprocess.run([helper, wheel, "CENTER", "build"], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_ordinary_helpers_use_two_independent_profiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("scripts", "config", "bin", "ordinary_caster/config", "ball_caster"):
                (root / name).mkdir(parents=True, exist_ok=True)
            for name in ("scripts/idf.sh", "ordinary_caster/idf.sh", "ball_caster/idf.sh"):
                shutil.copy2(REPO / name, root / name)
                (root / name).chmod(0o755)
            fake = root / "bin" / "idf.py"
            fake.write_text('#!/usr/bin/env python3\nimport json, sys\n'
                            'print("ESP-IDF v5.4.2" if "--version" in sys.argv else json.dumps(sys.argv[1:]))\n')
            fake.chmod(0o755)
            env = {**os.environ, "PATH": str(root / "bin") + os.pathsep + os.environ["PATH"]}
            for wheel in ("front_left", "front_right"):
                # A ball header cannot satisfy a missing ordinary configuration.
                (root / "config" / f"{wheel}.local.h").write_text("// ball only\n")
                helper = str(root / "ordinary_caster" / "idf.sh")
                missing = subprocess.run([helper, wheel, "SWIVEL"], env=env, capture_output=True, text=True)
                self.assertEqual(missing.returncode, 2)
                self.assertIn(f"ordinary_caster/config/{wheel}.local.h", missing.stderr)
                (root / "ordinary_caster/config" / f"{wheel}.local.h").write_text("// ordinary\n")
                for role in ("SWIVEL", "WHEEL", "ORDINARY"):
                    result = subprocess.run([helper, wheel, role, "build"], env=env,
                                            capture_output=True, text=True, check=True)
                    args = json.loads(result.stdout)
                    build = f"build/ordinary_caster/{wheel}" + ("" if role == "ORDINARY" else f"/{role}")
                    self.assertEqual(args[:2], ["-B", build])
                    self.assertIn(f"-DSDKCONFIG={root}/{build}/sdkconfig", args)
                    self.assertIn(f"-DCASTER_WHEEL={wheel}", args)
                    self.assertIn(f"-DCASTER_ROLE={role}", args)
                    for override in (("-DCASTER_ROLE=CENTER",), ("--build-dir=build/wrong",)):
                        rejected = subprocess.run([helper, wheel, role, *override], env=env,
                                                  capture_output=True, text=True)
                        self.assertEqual(rejected.returncode, 2)
                self.assertEqual(subprocess.run([helper, wheel, "build"], env=env,
                    capture_output=True).returncode, 2)
            rejected = subprocess.run([str(root / "ball_caster/idf.sh"), "front_left", "ORDINARY"],
                                      env=env, capture_output=True, text=True)
            self.assertEqual(rejected.returncode, 2)

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

    def test_ordinary_calibration_and_family_guards(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copy2(REPO / "ordinary_caster/config/example.h", root / "example.h")
            for index, wheel in enumerate(("front_left", "front_right"), 1):
                (root / f"{wheel}.local.h").write_text(
                    f'#define UDP_PORT {5006 + index}\n#define SWIVEL_SIGN {(-1 if index == 1 else 1)}\n'
                    f'#define WHEEL_SIGN {(1 if index == 1 else -1)}\n#define WHEEL_CPR {128 * index}\n'
                    f'#define ORDINARY_IPV4 "192.0.2.{index}4"\n#include "example.h"\n')
            source = root / "probe.cpp"
            source.write_text('#include "config.h"\n#include <iostream>\nint main() { '
                              'auto c = caster::configuration(); '
                              'std::cout << caster::kCasterType << " " << caster::kWheel << " " '
                              '<< caster::kRole << " " << UDP_PORT << " " << c.sign << " " '
                              '<< c.wheel_sign << " " << c.wheel_cpr << " " << c.wheel_cpr_verified '
                              '<< " " << c.max_rpm_verified << " " << c.phase_reference_valid '
                              '<< " " << c.wheel_velocity_window_us << " " << caster::kLocalIpv4; }\n')
            for index, wheel in enumerate(("front_left", "front_right"), 1):
                binary = root / f"ordinary-{wheel}"
                command = [os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           "-I", str(REPO / "main"), "-I", str(root), "-DCASTER_NODE_ID=4",
                           f"-DCASTER_WHEEL_INDEX={index}", f'-DCASTER_CONFIG_HEADER="{wheel}.local.h"',
                           str(source), "-o", str(binary)]
                subprocess.run(command, check=True, capture_output=True, text=True)
                values = subprocess.check_output([str(binary)], text=True).split()
                self.assertEqual(values, ["ordinary", wheel, "ORDINARY", str(5006 + index),
                                          str(-1 if index == 1 else 1), str(1 if index == 1 else -1),
                                          str(128 * index), "0", "0", "0", "50000", f"192.0.2.{index}4"])
                # Header family and build role must agree, independent of include search paths.
                wrong_family = ["-DCASTER_NODE_ID=3" if arg == "-DCASTER_NODE_ID=4" else arg for arg in command]
                rejected = subprocess.run(wrong_family, capture_output=True, text=True)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("Ball profiles must not use", rejected.stderr)
                invalid_window = subprocess.run([*command, "-DWHEEL_VELOCITY_WINDOW_US=1000"],
                                                capture_output=True, text=True)
                self.assertNotEqual(invalid_window.returncode, 0)
                self.assertIn("at least one acquisition interval", invalid_window.stderr)
            (root / "example.h").write_text((REPO / "config/example.h").read_text())
            rejected = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("Ordinary roles require their own", rejected.stderr)


if __name__ == "__main__":
    unittest.main()

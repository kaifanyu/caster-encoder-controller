#!/usr/bin/env python3
"""Build in WSL and flash through an existing Windows PlatformIO esptool."""

import argparse
import json
import ntpath
from pathlib import Path
import re
import shlex
import subprocess
import sys


def capture(*command):
    return subprocess.check_output(command, text=True).strip()


def windows_path(path):
    return capture("wslpath", "-w", str(path))


def linux_path(path):
    return Path(capture("wslpath", "-u", path))


def validate_profile(build, wheel, role):
    """Refuse artifacts configured for a different physical board."""
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(("CASTER_WHEEL:", "CASTER_ROLE:")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if cache.get("CASTER_WHEEL") != wheel or cache.get("CASTER_ROLE") != role:
        raise ValueError(f"build profile does not match {wheel}/{role}; rebuild this profile")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheel", choices=("front_left", "front_right"))
    parser.add_argument("role", choices=("H1", "H2", "CENTER", "ORDINARY", "SWIVEL", "WHEEL"))
    parser.add_argument("--port", required=True, help="Windows serial port, e.g. COM6")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate existing artifacts and print command; do not build or flash")
    parser.add_argument("--skip-build", action="store_true",
                        help="flash existing artifacts without rebuilding")
    parser.add_argument("--windows-python", help="Windows Python executable path")
    parser.add_argument("--esptool-path", help="Windows esptool.py path")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"COM[1-9][0-9]*", args.port, re.IGNORECASE):
        parser.error("--port must be a Windows COM port, e.g. COM6")

    repo = Path(__file__).resolve().parent.parent
    if not args.dry_run and not args.skip_build:
        subprocess.run([str(repo / "scripts/idf.sh"), args.wheel, args.role, "build"], check=True)
    build = (repo / "build" / "ordinary_caster" / args.wheel if args.role == "ORDINARY"
             else repo / "build" / args.wheel / args.role)
    if args.role in ("SWIVEL", "WHEEL"):
        build = repo / "build" / "ordinary_caster" / args.wheel / args.role
    manifest_path = build / "flasher_args.json"
    if not manifest_path.is_file():
        parser.error(f"missing {manifest_path}; run ./scripts/idf.sh {args.wheel} {args.role} build first")
    validate_profile(build, args.wheel, args.role)
    manifest = json.loads(manifest_path.read_text())
    extra = manifest["extra_esptool_args"]
    if extra["chip"] != "esp32c6":
        parser.error("expected an esp32c6 build")
    flash_args = manifest["write_flash_args"]
    if not isinstance(flash_args, list) or not all(isinstance(x, str) for x in flash_args):
        parser.error("invalid write_flash_args in build manifest")
    if any("erase" in argument.lower() for argument in flash_args):
        parser.error("refusing erase options in build manifest")
    flash_files = []
    for offset, filename in manifest["flash_files"].items():
        if not re.fullmatch(r"0x[0-9a-fA-F]+", offset):
            parser.error(f"invalid flash offset: {offset}")
        artifact = (build / filename).resolve()
        if not artifact.is_file() or not artifact.is_relative_to(build.resolve()):
            parser.error(f"missing or out-of-build artifact: {artifact}")
        flash_files.extend([offset, windows_path(artifact)])
    if not flash_files:
        parser.error("build manifest contains no flash files")

    if not args.windows_python or not args.esptool_path:
        profile = capture("powershell.exe", "-NoProfile", "-NonInteractive",
                          "-Command", "$env:USERPROFILE")
        platformio = ntpath.join(profile, ".platformio")
    python = args.windows_python or ntpath.join(platformio, "penv", "Scripts", "python.exe")
    esptool = args.esptool_path or ntpath.join(platformio, "packages", "tool-esptoolpy", "esptool.py")
    python_exe = linux_path(python) if not python.startswith("/") else Path(python)
    esptool_win = windows_path(Path(esptool)) if esptool.startswith("/") else esptool
    if not python_exe.is_file() or not linux_path(esptool_win).is_file():
        parser.error("Windows Python/esptool missing; specify --windows-python and --esptool-path")
    command = [str(python_exe), esptool_win, "--chip", extra["chip"],
               "--port", args.port.upper(), "--baud", "460800",
               "--before", extra.get("before", "default_reset"),
               "--after", extra.get("after", "hard_reset"),
               "write_flash", *flash_args, *flash_files]
    print(shlex.join(command), flush=True)
    if args.dry_run:
        print("Dry run: existing artifacts validated; no build, reset, or serial access.")
        return
    subprocess.run(command, check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"Flash setup failed: {error}", file=sys.stderr)
        sys.exit(1)

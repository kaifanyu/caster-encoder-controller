#!/usr/bin/env python3
"""Run the UDP logger on Windows from WSL so ESP32 packets avoid WSL NAT."""

import argparse
import ipaddress
import math
import ntpath
from pathlib import Path
import shlex
import subprocess
import sys

from flash_windows import capture, linux_path, windows_path


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", choices=("front_left", "front_right"), required=True)
    parser.add_argument("--port", type=int, help="UDP port (default: left 5005, right 5006)")
    parser.add_argument("--duration", type=float, help="stop after this many seconds; otherwise Ctrl+C")
    parser.add_argument("--output", type=Path, default=repo / "recordings" / "wifi",
                        help="recording parent directory (default: recordings/wifi in this repository)")
    parser.add_argument("--bind", default="0.0.0.0", help="Windows IPv4 listen address (default: all interfaces)")
    parser.add_argument("--windows-python", help="Windows Python executable path")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the command without opening a socket or creating recordings")
    args = parser.parse_args()
    if args.port is None:
        args.port = 5005 if args.wheel == "front_left" else 5006
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be finite and positive")
    try:
        ipaddress.IPv4Address(args.bind)
    except ipaddress.AddressValueError:
        parser.error("--bind must be an IPv4 address")

    python = args.windows_python
    if not python:
        profile = capture("powershell.exe", "-NoProfile", "-NonInteractive",
                          "-Command", "$env:USERPROFILE")
        python = ntpath.join(profile, ".platformio", "penv", "Scripts", "python.exe")
    python_exe = Path(python) if python.startswith("/") else linux_path(python)
    logger = repo / "host" / "logger.py"
    output = args.output.expanduser().resolve()
    if not python_exe.is_file() or not logger.is_file():
        parser.error("Windows Python or host/logger.py missing; check --windows-python")
    if output.exists() and not output.is_dir():
        parser.error("--output must be a directory")
    command = [str(python_exe), "-u", windows_path(logger),
               "--bind", args.bind, "--port", str(args.port),
               "--wheel", args.wheel,
               "--output", windows_path(output)]
    if args.duration is not None:
        command.extend(["--duration", str(args.duration)])
    print(shlex.join(command), flush=True)
    if args.dry_run:
        print("Dry run: no socket opened and no recordings created.")
        return
    subprocess.run(command, check=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Windows logger failed: {error}", file=sys.stderr)
        sys.exit(1)

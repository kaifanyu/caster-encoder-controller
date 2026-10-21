#!/usr/bin/env python3
"""Launch the Windows UDP-to-WSL relay without changing encoder payloads."""

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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-ip", required=True, type=ipaddress.IPv4Address,
                        help="WSL receiver IPv4 address (e.g. hostname -I in WSL)")
    parser.add_argument("--listen-port", type=int, default=5005)
    parser.add_argument("--target-port", type=int, default=5005)
    parser.add_argument("--duration", type=float, help="stop after this many seconds; otherwise Ctrl+C")
    parser.add_argument("--windows-python", help="Windows Python executable path")
    parser.add_argument("--dry-run", action="store_true", help="print command without opening sockets")
    args = parser.parse_args()
    if not all(1 <= port <= 65535 for port in (args.listen_port, args.target_port)):
        parser.error("ports must be in 1..65535")
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be finite and positive")
    if args.target_ip.is_unspecified or args.target_ip.is_multicast or str(args.target_ip) == "255.255.255.255":
        parser.error("--target-ip must be a unicast destination")
    if args.target_ip.is_loopback and args.listen_port == args.target_port:
        parser.error("loopback destination requires a different target port to prevent a relay loop")

    python = args.windows_python
    if not python:
        profile = capture("powershell.exe", "-NoProfile", "-NonInteractive",
                          "-Command", "$env:USERPROFILE")
        python = ntpath.join(profile, ".platformio", "penv", "Scripts", "python.exe")
    python_exe = Path(python) if python.startswith("/") else linux_path(python)
    relay = Path(__file__).resolve().parent.parent / "host" / "udp_relay.py"
    if not python_exe.is_file() or not relay.is_file():
        parser.error("Windows Python or host/udp_relay.py missing; check --windows-python")
    command = [str(python_exe), "-u", windows_path(relay), "--target-ip", str(args.target_ip),
               "--listen-port", str(args.listen_port), "--target-port", str(args.target_port)]
    if args.duration is not None:
        command.extend(["--duration", str(args.duration)])
    print(shlex.join(command), flush=True)
    if args.dry_run:
        print("Dry run: no sockets opened.")
        return
    subprocess.run(command, check=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Windows relay failed: {error}", file=sys.stderr)
        sys.exit(1)

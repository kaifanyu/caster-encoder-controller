#!/usr/bin/env python3
"""Record ESP32 telemetry in a ROS bag through the Windows-to-WSL UDP relay.

Starts only the passive encoder bridge, relay, and existing HAMR bag recorder.
Stop with Ctrl+C, or supply --duration to finalize the bag automatically.
"""

import argparse
from datetime import datetime
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parent.parent


def capture(command, **kwargs):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=True, **kwargs)
    return result.stdout


def stop_child(process, timeout):
    if process is None or process.poll() is not None:
        return
    for stop_signal, wait in ((signal.SIGINT, timeout), (signal.SIGTERM, 3), (signal.SIGKILL, 3)):
        try:
            os.killpg(process.pid, stop_signal)
            process.wait(timeout=wait)
            return
        except ProcessLookupError:
            return
        except subprocess.TimeoutExpired:
            continue
    print(f"Child process {process.pid} did not exit after cleanup", file=sys.stderr)


def main():
    defaults = next((path for path in (Path("/opt/ros/jazzy/setup.bash"),
                                      Path("/opt/ros/humble/setup.bash")) if path.is_file()), None)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", choices=("front_left", "front_right"), required=True)
    parser.add_argument("--port", type=int, help="UDP port (default: left 5005, right 5006)")
    parser.add_argument("--config", type=Path,
                        help="wheel inventory YAML; default: hamr_bringup/config/caster_encoders.yaml")
    parser.add_argument("--ros-setup", type=Path, default=defaults)
    parser.add_argument("--hamr-control", type=Path, default=REPO.parent / "hamr_control")
    parser.add_argument("--duration", type=float, help="record for this many seconds; default: until Ctrl+C")
    parser.add_argument("--output", type=Path, help="new bag directory; default: recordings/rosbags/caster_TIMESTAMP")
    parser.add_argument("--storage", choices=("auto", "mcap", "sqlite3"), default="auto")
    parser.add_argument("--domain-id", type=int, default=os.environ.get("ROS_DOMAIN_ID", "0"))
    parser.add_argument("--expected-node-ids", type=int, choices=(1, 2, 3), nargs="+", default=[1, 2, 3])
    args = parser.parse_args()
    if args.port is None:
        args.port = 5005 if args.wheel == "front_left" else 5006
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be finite and positive")
    if not 0 <= args.domain_id <= 232:
        parser.error("--domain-id must be in 0..232")
    if args.ros_setup is None or not args.ros_setup.is_file():
        parser.error("ROS setup missing; supply --ros-setup /opt/ros/DISTRO/setup.bash")
    scripts = args.hamr_control.resolve() / "hamr_bringup" / "scripts"
    bridge, recorder = scripts / "caster_encoder_bridge", scripts / "record_hamr_test_bag"
    relay = REPO / "scripts" / "relay_windows.py"
    qos = scripts.parent / "config" / "record_qos.yaml"
    config = (args.config or scripts.parent / "config" / "caster_encoders.yaml").resolve()
    for path in (bridge, recorder, relay, qos, config):
        if not path.is_file():
            parser.error(f"Required file missing: {path}")
    destination = (args.output or REPO / "recordings" / "rosbags" /
                   datetime.now().strftime("caster_%Y%m%d_%H%M%S_%f")).resolve()
    if destination.exists():
        parser.error(f"Bag output already exists: {destination}; choose a new --output")

    children = {}
    result = 0
    try:
        # Positional shell argument avoids interpolating paths as shell code.
        raw_env = capture(["bash", "-c", 'source "$1" >/dev/null && env -0',
                           "record_rosbag", str(args.ros_setup.resolve())])
        env = dict(os.fsdecode(entry).split("=", 1) for entry in raw_env.split(b"\0") if b"=" in entry)
        env["ROS_DOMAIN_ID"] = str(args.domain_id)
        ros_python = "/usr/bin/python3"
        available = json.loads(capture([ros_python, "-c",
            "import json, rosbag2_py; print(json.dumps(sorted(rosbag2_py.get_registered_writers())))"],
            env=env, text=True))
        storage = ("mcap" if "mcap" in available else "sqlite3") if args.storage == "auto" else args.storage
        if storage not in available:
            raise RuntimeError(f"ROS storage plugin {storage!r} is unavailable; installed: {available}")
        # Route to the current WSL gateway to obtain this instance's IPv4;
        # neither the WSL receiver address nor the Windows adapter is fixed.
        routes = json.loads(capture(["ip", "-j", "route", "show", "default"], text=True))
        gateway = next((route.get("gateway") for route in routes if route.get("gateway")), None)
        if gateway is None:
            raise RuntimeError("Cannot find the WSL default gateway for relay routing")
        route = json.loads(capture(["ip", "-j", "route", "get", gateway], text=True))[0]
        target = route.get("prefsrc") or route.get("src")
        if not target:
            raise RuntimeError("Cannot determine the WSL receiver IPv4 from its route")
        destination.parent.mkdir(parents=True, exist_ok=True)
        env.update(HAMR_BAG_ROOT=str(destination.parent), HAMR_BAG_NAME=destination.name,
                   HAMR_BAG_STORAGE_ID=storage, STORAGE_ID=storage,
                   HAMR_BAG_QOS_OVERRIDES=str(qos))
        print(f"Bag: {destination}\nStorage: {storage}; ROS domain: {args.domain_id}\n"
              f"Wheel: {args.wheel}; relay: Windows UDP {args.port} -> WSL {target}:{args.port}\n"
              "Passive recording only. Ctrl+C finalizes the bag.", flush=True)
        commands = {
            "recorder": ["bash", str(recorder), destination.name, str(destination)],
            "bridge": [ros_python, str(bridge), "--ros-args",
                       "-r", f"__node:=caster_encoder_{args.wheel}", "--params-file", str(config),
                       "-p", f"wheel_id:={args.wheel}", "-p", "require_config:=true",
                       "-p", "legacy_mode:=false", "-p", f"udp_port:={args.port}",
                       "-p", "expected_node_ids:=[" + ",".join(map(str, args.expected_node_ids)) + "]"],
            "relay": [sys.executable, str(relay), "--target-ip", target,
                      "--listen-port", str(args.port), "--target-port", str(args.port)],
        }
        if args.duration is not None:
            commands["relay"].extend(["--duration", str(args.duration + 10)])
        for name, command in commands.items():
            children[name] = subprocess.Popen(command, env=env, start_new_session=True)
        deadline = None if args.duration is None else time.monotonic() + args.duration
        while deadline is None or time.monotonic() < deadline:
            for name, process in children.items():
                if process.poll() is not None:
                    raise RuntimeError(f"{name} exited early with status {process.returncode}; see its output above")
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Recording failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(os.fsdecode(error.stderr).strip(), file=sys.stderr)
        result = 1
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        for name, timeout in (("recorder", 15), ("bridge", 5), ("relay", 5)):
            stop_child(children.get(name), timeout)
    if children:
        print(f"Recording stopped. Bag: {destination}", flush=True)
    return result


def interrupted(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    raise SystemExit(main())

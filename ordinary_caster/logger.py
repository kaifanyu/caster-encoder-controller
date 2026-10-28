"""Record ordinary caster v3 UDP telemetry without ROS or third-party packages.

Run from the repository root: python -m ordinary_caster.logger --wheel front_left
"""

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import socket
import tempfile
import time

from host.logger import SequenceTracker
from .protocol import CSV_FIELDS, IdentityGuard, WHEELS, channel_rows, decode_packet, identity


class Recorder:
    def __init__(self, output, wheel=None):
        if wheel is not None and wheel not in WHEELS:
            raise ValueError("wheel must be front_left or front_right")
        self.wheel = wheel
        self.guard = IdentityGuard()
        self.streams = {}
        self.valid_packets = self.invalid_packets = 0
        self.identity_conflicts = self.filtered_packets = self.channel_rows = 0
        self.started_epoch_ns = time.time_ns()
        self.closed = False
        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        self.path = Path(tempfile.mkdtemp(
            prefix=datetime.now(timezone.utc).strftime("run-%Y%m%dT%H%M%SZ-"), dir=output))
        self.packet_file = (self.path / "packets.jsonl").open("w", encoding="utf-8")
        self.metadata_file = (self.path / "metadata.jsonl").open("w", encoding="utf-8")
        self.csv_file = (self.path / "samples.csv").open("w", encoding="utf-8", newline="")
        self.writer = csv.DictWriter(self.csv_file, fieldnames=CSV_FIELDS, extrasaction="ignore")
        self.writer.writeheader()

    def receive(self, data, source, receive_epoch_ns, receive_monotonic_ns):
        try:
            packet = decode_packet(data)
        except ValueError:
            self.invalid_packets += 1
            return False
        key = identity(packet)
        if self.wheel is not None and packet["wheel_id"] != self.wheel:
            self.filtered_packets += 1
            return False
        try:
            self.guard.accept(packet)
        except ValueError:
            self.identity_conflicts += 1
            self.invalid_packets += 1
            return False
        envelope = dict(receive_epoch_ns=receive_epoch_ns,
                        receive_monotonic_ns=receive_monotonic_ns,
                        source_ip=source[0], source_port=source[1], packet=packet)
        serialized = json.dumps(envelope, allow_nan=False, separators=(",", ":")) + "\n"
        self.packet_file.write(serialized)
        self.valid_packets += 1
        if packet["type"] == "metadata":
            self.metadata_file.write(serialized)
            return True
        tracker = self.streams.setdefault(key, SequenceTracker())
        for sample in packet["samples"]:
            status = tracker.observe(sample["seq"])
            for row in channel_rows(packet, sample):
                row.update({name: envelope[name] for name in CSV_FIELDS[:4]})
                row["sequence_status"] = status
                self.writer.writerow(row)
                self.channel_rows += 1
        return True

    def flush(self):
        for stream in (self.packet_file, self.metadata_file, self.csv_file):
            stream.flush()
        summary = dict(
            caster_type="ordinary", started_epoch_ns=self.started_epoch_ns,
            updated_epoch_ns=time.time_ns(), valid_packets=self.valid_packets,
            invalid_packets=self.invalid_packets, identity_conflicts=self.identity_conflicts,
            filtered_packets=self.filtered_packets, wheel_filter=self.wheel,
            channel_rows=self.channel_rows,
            assignments=[dict(wheel_id=wheel, node_id=node, device_id=device)
                         for (wheel, node), device in sorted(self.guard.assignments.items())],
            streams=[dict(wheel_id=wheel, device_id=device, node_id=node, boot_id=boot,
                          **tracker.summary())
                     for (wheel, device, node, boot), tracker in sorted(self.streams.items())],
            timing_note="t_us is each channel's ESP acquisition time; receiver time is not synchronization.",
        )
        temporary = self.path / "stats.json.tmp"
        temporary.write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        temporary.replace(self.path / "stats.json")

    def close(self):
        if not self.closed:
            try:
                self.flush()
            finally:
                for stream in (self.packet_file, self.metadata_file, self.csv_file):
                    stream.close()
                self.closed = True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", choices=WHEELS, help="accept only this caster location")
    parser.add_argument("--port", type=int, help="UDP port (5007 left/default, 5008 right)")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--output", type=Path, default=Path("recordings/ordinary_caster"))
    parser.add_argument("--duration", type=float, help="seconds; otherwise record until Ctrl+C")
    args = parser.parse_args(argv)
    port = args.port if args.port is not None else (5008 if args.wheel == "front_right" else 5007)
    if not 1 <= port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be finite and positive")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            receiver.bind((args.bind, port))
            recorder = Recorder(args.output, wheel=args.wheel)
            print(f"Listening on UDP {args.bind}:{port}; recording to {recorder.path}", flush=True)
            started = flushed = time.monotonic()
            try:
                while True:
                    remaining = None if args.duration is None else args.duration - (time.monotonic() - started)
                    if remaining is not None and remaining <= 0:
                        break
                    receiver.settimeout(min(.25, remaining) if remaining is not None else .25)
                    try:
                        data, source = receiver.recvfrom(65535)
                        recorder.receive(data, source, time.time_ns(), time.monotonic_ns())
                    except socket.timeout:
                        pass
                    if time.monotonic() - flushed >= 1:
                        recorder.flush()
                        flushed = time.monotonic()
            except KeyboardInterrupt:
                pass
            finally:
                recorder.close()
            print(f"Saved {recorder.valid_packets} packets and {recorder.channel_rows} channel rows; "
                  f"rejected {recorder.invalid_packets}; filtered {recorder.filtered_packets}.")
    except OSError as error:
        parser.exit(2, f"ordinary logger: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

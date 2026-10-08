#!/usr/bin/env python3
"""Receive version-1 encoder UDP telemetry using only Python's standard library."""

import argparse
import csv
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, localcontext
import json
import math
from pathlib import Path
import re
import socket
import tempfile
import time


MAX_DATAGRAM = 65507
INT64_MAX = (1 << 63) - 1
UINT64_MAX = (1 << 64) - 1
BOOT_ID = re.compile(r"[0-9a-fA-F]{16}\Z")
INTEGER_TEXT = re.compile(r"-?(0|[1-9][0-9]*)\Z")
SAMPLE_FIELDS = (
    "seq", "t_us", "raw_count", "raw_spi_word", "position14", "angle_rad",
    "absolute_angle_rad", "calibrated_alpha_rad", "velocity_rad_s", "filtered_velocity_rad_s",
    "index_event_count", "index_timestamp_us", "index_raw_count", "flags",
)
DIAGNOSTIC_FIELDS = (
    "acquisition_overruns", "queue_drops", "invalid_spi_responses",
    "index_queue_drops", "radio_failures",
)
FLAG_NAMES = (
    "ready", "sample_valid", "ppr_verified", "phase_reference_valid",
    "unwrap_continuous", "clock_sync_valid", "velocity_valid", "parity_error",
    "continuity_lost", "index_valid", "rate_bound_verified",
    "acquisition_overrun", "spi_io_error",
)
CSV_FIELDS = (
    "receive_epoch_ns", "receive_monotonic_ns", "source_ip", "source_port",
    "node_id", "boot_id", *SAMPLE_FIELDS, *FLAG_NAMES,
    "sequence_status", "mapped_epoch_ns", "mapped_clock_sync_valid",
    *DIAGNOSTIC_FIELDS,
)


def integer(value, name, minimum=0, maximum=UINT64_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")
    return value


def finite_float(value):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("non-finite JSON number")
    return number


def reject_constant(value):
    raise ValueError(f"invalid JSON constant: {value}")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(data):
    result = json.loads(data, parse_constant=reject_constant,
                        parse_float=finite_float, object_pairs_hook=unique_object)
    # Reserve ample recursion headroom for the recording envelope and reject
    # gratuitous nesting in unknown metadata fields before writing to disk.
    pending = [(result, 0)]
    while pending:
        value, depth = pending.pop()
        if depth > 16:
            raise ValueError("JSON nesting exceeds 16 levels")
        if isinstance(value, dict):
            pending.extend((item, depth + 1) for item in value.values())
        elif isinstance(value, list):
            pending.extend((item, depth + 1) for item in value)
    return result


def identity(packet):
    integer(packet.get("node_id"), "node_id", 1, 3)
    if not isinstance(packet.get("boot_id"), str) or not BOOT_ID.fullmatch(packet["boot_id"]):
        raise ValueError("boot_id must be 16 hexadecimal characters")


def count_text(value, name):
    if value is None:
        return
    if not isinstance(value, str) or len(value) > 20 or not INTEGER_TEXT.fullmatch(value):
        raise ValueError(f"{name} must be a signed int64 decimal string or null")
    integer(int(value), name, -(1 << 63), INT64_MAX)


def decode_packet(data):
    """Validate one complete datagram; malformed input raises ValueError."""
    try:
        if not 0 < len(data) <= MAX_DATAGRAM:
            raise ValueError("empty or oversized datagram")
        packet = parse_json(data.decode("utf-8"))
        if not isinstance(packet, dict):
            raise ValueError("packet must be a JSON object")
        if type(packet.get("v")) is not int or packet["v"] != 1:
            raise ValueError("unsupported protocol version")
        identity(packet)
        if packet.get("type") == "metadata":
            expected_role = {1: "H1", 2: "H2", 3: "CENTER"}[packet["node_id"]]
            if packet.get("role") != expected_role:
                raise ValueError("metadata role disagrees with node_id")
            for name in ("firmware_version", "idf_version"):
                if not isinstance(packet.get(name), str) or not 1 <= len(packet[name]) <= 128:
                    raise ValueError(f"metadata requires {name}")
        elif packet.get("type") == "samples":
            samples = packet.get("samples")
            if not isinstance(samples, list) or not 1 <= len(samples) <= 32:
                raise ValueError("samples must contain 1 to 32 records")
            diagnostics = packet.get("diagnostics")
            if not isinstance(diagnostics, dict):
                raise ValueError("diagnostics must be an object")
            for name in DIAGNOSTIC_FIELDS:
                integer(diagnostics.get(name), name)
            for sample in samples:
                if not isinstance(sample, dict) or not all(k in sample for k in SAMPLE_FIELDS):
                    raise ValueError("sample is missing required fields")
                integer(sample["seq"], "seq")
                integer(sample["t_us"], "t_us", 0, INT64_MAX)
                integer(sample["flags"], "flags", 0, (1 << 13) - 1)
                integer(sample["index_event_count"], "index_event_count")
                integer(sample["index_timestamp_us"], "index_timestamp_us", 0, INT64_MAX)
                for name in ("raw_count", "index_raw_count"):
                    count_text(sample[name], name)
                for name, upper in (("raw_spi_word", 65535), ("position14", 16383)):
                    if sample[name] is not None:
                        integer(sample[name], name, 0, upper)
                for name in ("angle_rad", "absolute_angle_rad", "calibrated_alpha_rad", "velocity_rad_s", "filtered_velocity_rad_s"):
                    value = sample[name]
                    if value is not None:
                        if type(value) not in (int, float):
                            raise ValueError(f"{name} must be a finite number or null")
                        finite_float(value)
        else:
            raise ValueError("unknown packet type")
        return packet
    except (UnicodeError, RecursionError, OverflowError, TypeError) as error:
        raise ValueError(f"invalid packet: {error}") from error


class SequenceTracker:
    """Track gaps as intervals, including late recovery without storing every sample."""

    def __init__(self):
        self.highest_seq = -1
        self.unique_samples = 0
        self.duplicate_samples = 0
        self.reordered_samples = 0
        self.recovered_samples = 0
        self.missing_samples = 0
        self.gaps = []

    def observe(self, sequence):
        if sequence > self.highest_seq:
            gap = sequence - self.highest_seq - 1
            if gap:
                self.gaps.append((self.highest_seq + 1, sequence - 1))
                self.missing_samples += gap
            self.highest_seq = sequence
            self.unique_samples += 1
            return "in_order"
        for index, (start, end) in enumerate(self.gaps):
            if start <= sequence <= end:
                replacement = []
                if start < sequence:
                    replacement.append((start, sequence - 1))
                if sequence < end:
                    replacement.append((sequence + 1, end))
                self.gaps[index:index + 1] = replacement
                self.unique_samples += 1
                self.reordered_samples += 1
                self.recovered_samples += 1
                self.missing_samples -= 1
                return "recovered"
            if start > sequence:
                break
        self.duplicate_samples += 1
        return "duplicate"

    def summary(self):
        return {name: getattr(self, name) for name in (
            "highest_seq", "unique_samples", "duplicate_samples", "reordered_samples",
            "recovered_samples", "missing_samples",
        )}


class ClockMapper:
    """Apply externally calibrated affine maps; never estimate from UDP arrival time."""

    def __init__(self, document=None):
        self.maps = {}
        if document is None:
            return
        if not isinstance(document, dict) or type(document.get("version")) is not int or document["version"] != 1:
            raise ValueError("clock map requires version: 1")
        if not isinstance(document.get("mappings"), list):
            raise ValueError("clock map requires mappings array")
        for entry in document["mappings"]:
            if not isinstance(entry, dict):
                raise ValueError("clock mapping must be an object")
            identity(entry)
            if type(entry.get("calibrated")) is not bool:
                raise ValueError("clock mapping requires explicit calibrated boolean")
            try:
                slope = Decimal(str(entry["slope_ns_per_us"]))
            except (KeyError, InvalidOperation) as error:
                raise ValueError("invalid clock slope") from error
            if not slope.is_finite() or not Decimal("1") <= slope <= Decimal("1000000"):
                raise ValueError("clock slope must be finite in [1, 1000000] ns/us")
            offset = integer(entry.get("offset_ns"), "offset_ns", -(1 << 127), (1 << 127) - 1)
            start = integer(entry.get("valid_from_us", 0), "valid_from_us", 0, INT64_MAX)
            end = integer(entry.get("valid_to_us", INT64_MAX), "valid_to_us", start, INT64_MAX)
            key = (entry["node_id"], entry["boot_id"].lower())
            if key in self.maps:
                raise ValueError("duplicate node/boot clock mapping")
            self.maps[key] = (entry["calibrated"], slope, offset, start, end)

    def map(self, node_id, boot_id, timestamp_us):
        mapping = self.maps.get((node_id, boot_id.lower()))
        if mapping is None:
            return None
        calibrated, slope, offset, start, end = mapping
        if not calibrated or not start <= timestamp_us <= end:
            return None
        with localcontext() as context:
            context.prec = 60
            return int((slope * timestamp_us + offset).to_integral_value())


class Recorder:
    """Write a distinct run directory; flush at least once per second in the CLI."""

    def __init__(self, output, clock_mapper=None, clock_document=None):
        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        prefix = datetime.now(timezone.utc).strftime("run-%Y%m%dT%H%M%SZ-")
        self.path = Path(tempfile.mkdtemp(prefix=prefix, dir=output))
        self.packet_file = (self.path / "packets.jsonl").open("w", encoding="utf-8")
        self.metadata_file = (self.path / "metadata.jsonl").open("w", encoding="utf-8")
        self.csv_file = (self.path / "samples.csv").open("w", encoding="utf-8", newline="")
        self.csv_writer = csv.DictWriter(self.csv_file, fieldnames=CSV_FIELDS)
        self.csv_writer.writeheader()
        self.clock_mapper = clock_mapper or ClockMapper()
        self.streams = {}
        self.valid_packets = 0
        self.invalid_packets = 0
        self.started_epoch_ns = time.time_ns()
        self.closed = False
        if clock_document is not None:
            (self.path / "clock-map.json").write_text(
                json.dumps(clock_document, indent=2) + "\n", encoding="utf-8")

    def receive(self, data, source, receive_epoch_ns, receive_monotonic_ns):
        try:
            packet = decode_packet(data)
        except ValueError:
            self.invalid_packets += 1
            return False
        envelope = {
            "receive_epoch_ns": receive_epoch_ns,
            "receive_monotonic_ns": receive_monotonic_ns,
            "source_ip": source[0], "source_port": source[1], "packet": packet,
        }
        self.packet_file.write(json.dumps(envelope, separators=(",", ":")) + "\n")
        self.valid_packets += 1
        if packet["type"] == "metadata":
            self.metadata_file.write(json.dumps(envelope, separators=(",", ":")) + "\n")
            return True
        key = (packet["node_id"], packet["boot_id"].lower())
        tracker = self.streams.setdefault(key, SequenceTracker())
        for sample in packet["samples"]:
            mapped = self.clock_mapper.map(*key, sample["t_us"])
            row = {name: envelope[name] for name in CSV_FIELDS[:4]}
            row.update(node_id=packet["node_id"], boot_id=packet["boot_id"])
            row.update({name: sample[name] for name in SAMPLE_FIELDS})
            row.update({name: int(bool(sample["flags"] & (1 << bit)))
                        for bit, name in enumerate(FLAG_NAMES)})
            row.update(sequence_status=tracker.observe(sample["seq"]),
                       mapped_epoch_ns=mapped, mapped_clock_sync_valid=int(mapped is not None))
            row.update({name: packet["diagnostics"][name] for name in DIAGNOSTIC_FIELDS})
            self.csv_writer.writerow(row)
        return True

    def flush(self):
        for handle in (self.packet_file, self.metadata_file, self.csv_file):
            handle.flush()
        summary = {
            "started_epoch_ns": self.started_epoch_ns, "updated_epoch_ns": time.time_ns(),
            "valid_packets": self.valid_packets, "invalid_packets": self.invalid_packets,
            "streams": [{"node_id": node, "boot_id": boot, **tracker.summary()}
                        for (node, boot), tracker in sorted(self.streams.items())],
        }
        temporary = self.path / "stats.json.tmp"
        temporary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.path / "stats.json")

    def close(self):
        if not self.closed:
            try:
                self.flush()
            finally:
                for handle in (self.packet_file, self.metadata_file, self.csv_file):
                    handle.close()
                self.closed = True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("recordings"), help="parent directory for a new recording")
    parser.add_argument("--bind", default="0.0.0.0", help="IPv4 listen address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=5005, help="UDP listen port (default: 5005)")
    parser.add_argument("--duration", type=float, help="stop after this many seconds; otherwise Ctrl+C")
    parser.add_argument("--clock-map", type=Path, help="optional externally calibrated per-boot affine mappings")
    arguments = parser.parse_args(argv)
    if not 1 <= arguments.port <= 65535:
        parser.error("--port must be in 1..65535")
    if arguments.duration is not None and (not math.isfinite(arguments.duration) or arguments.duration <= 0):
        parser.error("--duration must be finite and positive")
    document = None
    try:
        if arguments.clock_map:
            document = parse_json(arguments.clock_map.read_text(encoding="utf-8"))
        mapper = ClockMapper(document)
    except (OSError, ValueError, RecursionError) as error:
        parser.error(f"cannot load clock map: {error}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        receiver.bind((arguments.bind, arguments.port))
        recorder = Recorder(arguments.output, mapper, document)
        print(f"Listening on UDP {arguments.bind}:{arguments.port}; recording to {recorder.path}", flush=True)
        started = last_flush = time.monotonic()
        try:
            while True:
                remaining = None if arguments.duration is None else arguments.duration - (time.monotonic() - started)
                if remaining is not None and remaining <= 0:
                    break
                receiver.settimeout(min(0.25, remaining) if remaining is not None else 0.25)
                try:
                    data, source = receiver.recvfrom(65535)
                    epoch_ns, monotonic_ns = time.time_ns(), time.monotonic_ns()
                    recorder.receive(data, source, epoch_ns, monotonic_ns)
                except socket.timeout:
                    pass
                if time.monotonic() - last_flush >= 1:
                    recorder.flush()
                    last_flush = time.monotonic()
        except KeyboardInterrupt:
            pass
        finally:
            recorder.close()
        print(f"Saved {recorder.valid_packets} valid packets; rejected {recorder.invalid_packets} malformed packets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

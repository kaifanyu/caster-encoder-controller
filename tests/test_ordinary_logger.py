import copy
import csv
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

from ordinary_caster import protocol
from ordinary_caster.logger import Recorder


ROOT = Path(__file__).resolve().parents[1]
BOOT = "0123456789abcdef"
IDENTITY = dict(v=3, caster_type="ordinary", node_id=4, wheel_id="front_left",
                device_id="02:00:00:00:00:01", boot_id=BOOT)


def sample(sequence=0):
    return dict(
        seq=sequence, t_us=250100 + sequence * 5000,
        swivel=dict(raw_spi_word=0xC000, position12=0, absolute_angle_rad=0.0,
                    calibrated_angle_rad=0.0, angle_rad=None, velocity_rad_s=None,
                    filtered_velocity_rad_s=None, flags=3 | 256),
        wheel=dict(t_us=250000 + sequence * 5000, raw_count="9223372036854775807",
                   angle_rad=0.1, velocity_rad_s=None, filtered_velocity_rad_s=None,
                   flags=3 | 16),
    )


def packet(*sequences):
    return {**IDENTITY, "type": "samples",
            "samples": [sample(seq) for seq in sequences or (0,)],
            "diagnostics": {name: 0 for name in protocol.DIAGNOSTIC_FIELDS}}


def metadata():
    return {
        **IDENTITY, "type": "metadata", "role": "ORDINARY",
        "firmware_version": "0.4.0", "idf_version": "v5.4.2",
        "sample_hz": 200, "transmit_hz": 200, "transmission_task_hz": 100,
        "samples_per_packet": 1, "record_version": 2, "record_bytes": 116,
        "swivel": dict(encoder_type="AMT222A-V", resolution_bits=12, counts_per_rev=4096,
                       coordinate_sign=1, zero_rad=0.0, phase_reference_valid=False,
                       max_rpm=600.0, max_rpm_verified=False, velocity_filter_tau_s=0.0),
        "wheel": dict(counts_per_rev=128, coordinate_sign=1, software_zero_count="0",
                      cpr_verified=False, velocity_window_us=100000, velocity_filter_tau_s=0.0,
                      glitch_filter_ns=0, phase_reference_valid=False, index_available=False,
                      invalid_transition_detection=False),
    }


def encoded(value):
    return json.dumps(value).encode("utf-8")


class OrdinaryProtocolTests(unittest.TestCase):
    def test_round_trip_precise_counts_and_both_acquisition_times(self):
        source = packet()
        row = source["samples"][0]
        row["seq"] = 2**64 - 1
        row["t_us"] = 2**53 + 13
        row["wheel"]["t_us"] = 2**53 + 7
        self.assertEqual(protocol.decode_packet(encoded(source)), source)
        rows = list(protocol.channel_rows(source, row))
        self.assertEqual([item["t_us"] for item in rows], [2**53 + 13, 2**53 + 7])
        self.assertEqual(rows[1]["raw_count"], "9223372036854775807")
        self.assertIsNone(rows[0]["raw_count"])
        self.assertIsNone(rows[1]["position12"])

    def test_invalid_swivel_does_not_remove_valid_wheel_and_reverse(self):
        source = packet(0, 1)
        source["samples"][0]["swivel"].update(
            raw_spi_word=0x61AB, position12=None, absolute_angle_rad=None,
            calibrated_angle_rad=None, flags=1 | protocol.FORMAT_ERROR)
        source["samples"][1]["wheel"].update(raw_count=None, angle_rad=None, flags=1)
        actual = protocol.decode_packet(encoded(source))
        first = list(protocol.channel_rows(actual, actual["samples"][0]))
        second = list(protocol.channel_rows(actual, actual["samples"][1]))
        self.assertEqual([row["sample_valid"] for row in first], [0, 1])
        self.assertEqual(first[0]["format_error"], 1)
        self.assertEqual([row["sample_valid"] for row in second], [1, 0])

    def test_metadata_round_trip_and_wrong_family(self):
        source = metadata()
        source["custom_notes"] = {"preserve": True}
        self.assertEqual(protocol.decode_packet(encoded(source)), source)
        for key, value in (("v", 2), ("v", True), ("caster_type", "ball"),
                           ("node_id", 3), ("node_id", 4.0), ("role", "CENTER"),
                           ("wheel_id", "rear_left"), ("device_id", "invalid"),
                           ("boot_id", "bad"), ("record_bytes", 100),
                           ("samples_per_packet", 2), ("sample_hz", True),
                           ("swivel", None), ("wheel", {})):
            with self.subTest(key=key, value=value):
                wrong = copy.deepcopy(source)
                wrong[key] = value
                with self.assertRaises(ValueError):
                    protocol.decode_packet(encoded(wrong))

    def test_metadata_channel_types_and_limits(self):
        cases = (("swivel", "counts_per_rev", 16384), ("swivel", "resolution_bits", 14),
                 ("swivel", "coordinate_sign", 0), ("swivel", "zero_rad", "0.0"),
                 ("swivel", "max_rpm", 0), ("swivel", "max_rpm_verified", 1),
                 ("wheel", "counts_per_rev", 0), ("wheel", "cpr_verified", "false"),
                 ("wheel", "software_zero_count", 1), ("wheel", "software_zero_count", None),
                 ("wheel", "velocity_window_us", -1), ("wheel", "index_available", 0))
        for channel, field, value in cases:
            with self.subTest(channel=channel, field=field, value=value):
                wrong = metadata()
                wrong[channel][field] = value
                with self.assertRaises(ValueError):
                    protocol.decode_packet(encoded(wrong))

    def test_rejects_malformed_samples_and_json(self):
        cases = (("swivel", "position12", 4096), ("swivel", "position12", True),
                 ("swivel", "flags", 1 << 14), ("swivel", "raw_spi_word", None),
                 ("swivel", "raw_spi_word", -1), ("swivel", "angle_rad", float("nan")),
                 ("swivel", "absolute_angle_rad", float("inf")),
                 ("wheel", "raw_count", 2**53 + 1), ("wheel", "raw_count", "+1"),
                 ("wheel", "raw_count", "01"), ("wheel", "raw_count", str(2**63)),
                 ("wheel", "t_us", 2**63), ("wheel", "t_us", False),
                 ("wheel", "velocity_rad_s", "1.0"), ("wheel", "flags", True))
        for channel, field, value in cases:
            with self.subTest(channel=channel, field=field):
                wrong = packet()
                wrong["samples"][0][channel][field] = value
                with self.assertRaises(ValueError):
                    protocol.decode_packet(encoded(wrong))
        for wrong in (b"", b"null", b"[]", b"\xff", b'{"v":3,"v":3}',
                      encoded(packet()).replace(b'"seq": 0', b'"seq": -1'),
                      encoded(packet()).replace(b'"t_us": 250100', b'"t_us": 1e999')):
            with self.assertRaises(ValueError):
                protocol.decode_packet(wrong)


class OrdinaryRecorderTests(unittest.TestCase):
    def test_two_rows_per_sample_reboot_gaps_duplicate_and_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            def receive(value):
                return recorder.receive(encoded(value), ("127.0.0.1", 1234), 2**60 + 1, 2**53 + 7)
            self.assertTrue(receive(metadata()))
            self.assertTrue(receive(packet(0, 2, 1, 2)))
            reboot = packet()
            reboot["boot_id"] = "fedcba9876543210"
            self.assertTrue(receive(reboot))
            recorder.close()
            with (recorder.path / "samples.csv").open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 10)
            self.assertEqual([row["channel"] for row in rows[:2]], ["swivel", "wheel"])
            self.assertEqual([int(row["t_us"]) for row in rows[:2]], [250100, 250000])
            self.assertEqual(rows[1]["raw_count"], "9223372036854775807")
            self.assertEqual(rows[0]["receive_epoch_ns"], str(2**60 + 1))
            self.assertEqual([row["sequence_status"] for row in rows[::2]],
                             ["in_order", "in_order", "recovered", "duplicate", "in_order"])
            stats = json.loads((recorder.path / "stats.json").read_text())
            self.assertEqual(len(stats["streams"]), 2)
            first = next(row for row in stats["streams"] if row["boot_id"] == BOOT)
            self.assertEqual(first["unique_samples"], 3)
            self.assertEqual(first["duplicate_samples"], 1)
            self.assertEqual(first["missing_samples"], 0)
            self.assertEqual(len((recorder.path / "metadata.jsonl").read_text().splitlines()), 1)
            recorder.close()

    def test_family_filter_and_mac_assignments(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory, wheel="front_left")
            def receive(value):
                return recorder.receive(encoded(value), ("127.0.0.1", 1234), 1, 1)
            self.assertTrue(receive(packet()))
            wrong_wheel = packet()
            wrong_wheel["wheel_id"] = "front_right"
            self.assertFalse(receive(wrong_wheel))
            wrong_mac = packet()
            wrong_mac["device_id"] = "02:00:00:00:00:02"
            self.assertFalse(receive(wrong_mac))
            ball = packet()
            ball.update(v=2, caster_type="ball", node_id=3)
            self.assertFalse(receive(ball))
            self.assertEqual(recorder.filtered_packets, 1)
            self.assertEqual(recorder.identity_conflicts, 1)
            self.assertEqual(recorder.invalid_packets, 2)
            recorder.close()
            combined = Recorder(directory)
            self.assertTrue(combined.receive(encoded(packet()), ("127.0.0.1", 1), 1, 1))
            self.assertFalse(combined.receive(encoded(wrong_wheel), ("127.0.0.1", 1), 1, 1))
            wrong_wheel["device_id"] = "02:00:00:00:00:02"
            self.assertTrue(combined.receive(encoded(wrong_wheel), ("127.0.0.1", 1), 1, 1))
            combined.close()

    def test_real_udp_cli_saves_independent_channels_and_exits(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            process = subprocess.Popen(
                [sys.executable, "-m", "ordinary_caster.logger", "--bind", "127.0.0.1",
                 "--wheel", "front_left", "--port", str(port), "--duration", "0.8",
                 "--output", directory], cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            try:
                line = process.stdout.readline()
                self.assertIn("Listening on UDP", line)
                source = packet()
                source["samples"][0]["swivel"].update(
                    raw_spi_word=0, position12=None, absolute_angle_rad=None,
                    calibrated_angle_rad=None, flags=1 | 128)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    for value in (metadata(), source, source):
                        sender.sendto(encoded(value), ("127.0.0.1", port))
                    sender.sendto(b'{"v":3,"v":3}', ("127.0.0.1", port))
                stdout, stderr = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 0, stderr)
                runs = list(Path(directory).glob("run-*"))
                self.assertEqual(len(runs), 1)
                stats = json.loads((runs[0] / "stats.json").read_text())
                self.assertEqual(stats["valid_packets"], 3)
                self.assertEqual(stats["invalid_packets"], 1)
                self.assertEqual(stats["streams"][0]["duplicate_samples"], 1)
                with (runs[0] / "samples.csv").open(newline="") as stream:
                    rows = list(csv.DictReader(stream))
                self.assertEqual([row["sample_valid"] for row in rows], ["0", "1", "0", "1"])
                self.assertEqual(rows[0]["absolute_angle_rad"], "")
                self.assertEqual(rows[1]["raw_count"], "9223372036854775807")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()


if __name__ == "__main__":
    unittest.main()

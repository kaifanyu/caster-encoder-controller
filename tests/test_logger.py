import copy
import csv
import io
import json
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import unittest

from host.logger import ClockMapper, decode_packet, Recorder, SequenceTracker


BOOT = "0123456789abcdef"


def sample(sequence=0):
    return {
        "seq": sequence, "t_us": 250000 + sequence * 5000,
        "raw_count": "9223372036854775807", "raw_spi_word": None,
        "position14": None, "angle_rad": 0.0, "absolute_angle_rad": None,
        "calibrated_alpha_rad": None,
        "velocity_rad_s": None, "filtered_velocity_rad_s": None,
        "index_event_count": 0, "index_timestamp_us": 0, "index_raw_count": None,
        "flags": 3,
    }


def packet(*sequences):
    return {
        "v": 1, "type": "samples", "node_id": 1, "boot_id": BOOT,
        "samples": [sample(sequence) for sequence in (sequences or (0,))],
        "diagnostics": {
            "acquisition_overruns": 0, "queue_drops": 0,
            "invalid_spi_responses": 0, "index_queue_drops": 0, "radio_failures": 0,
        },
    }


def encoded(value):
    return json.dumps(value).encode("utf-8")


def clock_document(calibrated=True):
    return {"version": 1, "mappings": [{
        "node_id": 1, "boot_id": BOOT, "calibrated": calibrated,
        "slope_ns_per_us": "1000.125", "offset_ns": 1791480000123456789,
        "valid_from_us": 250000, "valid_to_us": 1000000,
    }]}


class DecoderTests(unittest.TestCase):
    def test_hemisphere_preserves_full_int64(self):
        source = packet(0, 1)
        source["samples"][1]["raw_count"] = "-9223372036854775808"
        self.assertEqual(decode_packet(encoded(source)), source)

    def test_center_sample_and_parity_failure_retain_raw_word(self):
        source = packet(7)
        source["node_id"] = 3
        source["samples"][0].update(raw_count=None, raw_spi_word=0x61AB, position14=8619,
                                    absolute_angle_rad=3.305, calibrated_alpha_rad=3.305, angle_rad=None)
        self.assertEqual(decode_packet(encoded(source))["samples"][0]["position14"], 8619)
        self.assertEqual(decode_packet(encoded(source))["samples"][0]["calibrated_alpha_rad"], 3.305)
        source["samples"][0].update(raw_spi_word=0xFFFF, position14=None,
                                    absolute_angle_rad=None, calibrated_alpha_rad=None,
                                    angle_rad=None, flags=1 | (1 << 7))
        self.assertEqual(decode_packet(encoded(source))["samples"][0]["raw_spi_word"], 65535)

    def test_metadata_retains_configuration(self):
        source = {"v": 1, "type": "metadata", "node_id": 1, "boot_id": BOOT,
                  "role": "H1", "firmware_version": "0.1.0", "idf_version": "v5.4.2",
                  "encoder_ppr": 2048, "ppr_verified": False}
        self.assertEqual(decode_packet(encoded(source)), source)
        source["role"] = "CENTER"
        with self.assertRaises(ValueError):
            decode_packet(encoded(source))

    def test_rejects_bad_envelopes(self):
        for field, value in (("node_id", True), ("node_id", 4), ("v", True), ("v", 2),
                             ("type", "unknown"), ("boot_id", "bad"), ("samples", []),
                             ("samples", [sample()] * 33), ("diagnostics", None)):
            with self.subTest(field=field, value=value):
                source = packet()
                source[field] = value
                with self.assertRaises(ValueError):
                    decode_packet(encoded(source))

    def test_rejects_malformed_sample_values(self):
        cases = (
            ("raw_count", 9007199254740993), ("raw_count", "9223372036854775808"),
            ("raw_count", "+1"), ("raw_count", "01"), ("index_raw_count", "bad"),
            ("raw_spi_word", 65536), ("position14", 16384), ("position14", True),
            ("seq", -1), ("seq", 1.0), ("t_us", 1 << 63), ("flags", 1 << 13),
            ("index_event_count", -1), ("index_timestamp_us", False),
            ("angle_rad", "0.0"), ("calibrated_alpha_rad", "0.0"), ("velocity_rad_s", True),
            ("angle_rad", float("inf")), ("angle_rad", float("nan")),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                source = packet()
                source["samples"][0][field] = value
                with self.assertRaises(ValueError):
                    decode_packet(encoded(source))
        source = packet()
        del source["samples"][0]["angle_rad"]
        with self.assertRaises(ValueError):
            decode_packet(encoded(source))

    def test_rejects_invalid_json_without_crashing_receiver(self):
        nested_metadata = {
            "v": 1, "type": "metadata", "node_id": 1, "boot_id": BOOT,
            "role": "H1", "firmware_version": "0.1.0", "idf_version": "v5.4.2",
        }
        nested_extra = []
        for _ in range(20):
            nested_extra = [nested_extra]
        nested_metadata["extra"] = nested_extra
        malformed = (
            b"", b"\xff", b"[]", b"null", b'{"v":1,"v":1}', b"{" * 5000,
            b"[" * 2000 + b"]" * 2000, b"x" * 65508,
            encoded(packet()).replace(b'"angle_rad": 0.0', b'"angle_rad": 1e309'),
            encoded(nested_metadata),
        )
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            try:
                for data in malformed:
                    with self.subTest(data=data[:30]):
                        self.assertFalse(recorder.receive(data, ("127.0.0.1", 1000), 1, 2))
                self.assertTrue(recorder.receive(encoded(packet()), ("127.0.0.1", 1000), 3, 4))
            finally:
                recorder.close()
            self.assertEqual(recorder.invalid_packets, len(malformed))


class SequenceTests(unittest.TestCase):
    def test_loss_duplicate_and_recovery(self):
        tracker = SequenceTracker()
        self.assertEqual([tracker.observe(seq) for seq in (0, 3, 1, 1, 2)],
                         ["in_order", "in_order", "recovered", "duplicate", "recovered"])
        self.assertEqual(tracker.summary(), {
            "highest_seq": 3, "unique_samples": 4, "duplicate_samples": 1,
            "reordered_samples": 2, "recovered_samples": 2, "missing_samples": 0,
        })
        self.assertEqual(tracker.gaps, [])

    def test_gap_intervals_split_and_large_jump_is_compact(self):
        tracker = SequenceTracker()
        tracker.observe(1_000_000_000)
        tracker.observe(50)
        self.assertEqual(tracker.gaps, [(0, 49), (51, 999_999_999)])
        self.assertEqual(tracker.missing_samples, 999_999_999)
        tracker.observe(0)
        tracker.observe(999_999_999)
        self.assertEqual(tracker.missing_samples, 999_999_997)


class ClockTests(unittest.TestCase):
    def test_default_is_unsynchronized(self):
        self.assertIsNone(ClockMapper().map(1, BOOT, 250000))
        self.assertIsNone(ClockMapper(clock_document(False)).map(1, BOOT, 250000))

    def test_affine_mapping_preserves_epoch_precision_and_checks_identity(self):
        mapper = ClockMapper(clock_document())
        self.assertEqual(mapper.map(1, BOOT, 250000), 1791480000373488039)
        self.assertIsNone(mapper.map(2, BOOT, 250000))
        self.assertIsNone(mapper.map(1, "aaaaaaaaaaaaaaaa", 250000))
        self.assertIsNone(mapper.map(1, BOOT, 249999))
        self.assertIsNone(mapper.map(1, BOOT, 1000001))

    def test_invalid_mapping_rejected(self):
        for field, value in (("calibrated", "true"), ("slope_ns_per_us", "NaN"),
                             ("slope_ns_per_us", "-1"), ("offset_ns", 1.5),
                             ("valid_to_us", 1)):
            document = clock_document()
            document["mappings"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                ClockMapper(document)
        document = clock_document()
        document["mappings"].append(copy.deepcopy(document["mappings"][0]))
        with self.assertRaises(ValueError):
            ClockMapper(document)


class RecorderTests(unittest.TestCase):
    def test_csv_jsonl_and_boot_separation(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            original = packet(0, 2)
            recorder.receive(encoded(original), ("10.0.0.2", 1234), 123456789, 222)
            recovered = packet(1)
            recorder.receive(encoded(recovered), ("10.0.0.2", 1234), 123456799, 232)
            reboot = packet(0)
            reboot["boot_id"] = "fedcba9876543210"
            recorder.receive(encoded(reboot), ("10.0.0.2", 1234), 123456809, 242)
            recorder.close()
            recorder.close()  # Closing is idempotent.
            rows = list(csv.DictReader(io.StringIO((recorder.path / "samples.csv").read_text())))
            self.assertEqual(len(rows), 4)
            self.assertEqual(rows[0]["raw_count"], "9223372036854775807")
            self.assertEqual(rows[0]["absolute_angle_rad"], "")
            self.assertEqual(rows[0]["calibrated_alpha_rad"], "")
            self.assertEqual(rows[0]["mapped_epoch_ns"], "")
            self.assertEqual(rows[0]["mapped_clock_sync_valid"], "0")
            self.assertEqual(rows[0]["clock_sync_valid"], "0")
            self.assertEqual(rows[2]["sequence_status"], "recovered")
            envelopes = [json.loads(line) for line in (recorder.path / "packets.jsonl").read_text().splitlines()]
            self.assertEqual(envelopes[0]["packet"], original)
            self.assertEqual(envelopes[0]["receive_epoch_ns"], 123456789)
            stats = json.loads((recorder.path / "stats.json").read_text())
            self.assertEqual(len(stats["streams"]), 2)
            self.assertEqual(stats["streams"][0]["missing_samples"], 0)
            self.assertEqual(stats["streams"][1]["highest_seq"], 0)
            second = Recorder(directory)
            second.close()
            self.assertNotEqual(second.path, recorder.path)

    def test_metadata_and_clock_map_are_saved(self):
        with tempfile.TemporaryDirectory() as directory:
            document = clock_document()
            recorder = Recorder(directory, ClockMapper(document), document)
            metadata = {"v": 1, "type": "metadata", "node_id": 1, "boot_id": BOOT,
                        "role": "H1", "firmware_version": "0.1.0", "idf_version": "v5.4.2"}
            recorder.receive(encoded(metadata), ("127.0.0.1", 5005), 1, 2)
            recorder.receive(encoded(packet()), ("127.0.0.1", 5005), 3, 4)
            recorder.close()
            row = next(csv.DictReader(io.StringIO((recorder.path / "samples.csv").read_text())))
            self.assertEqual(row["mapped_epoch_ns"], "1791480000373488039")
            self.assertEqual(row["mapped_clock_sync_valid"], "1")
            self.assertEqual(row["clock_sync_valid"], "0")
            self.assertEqual(json.loads((recorder.path / "clock-map.json").read_text()), document)
            saved = json.loads((recorder.path / "metadata.jsonl").read_text())
            self.assertEqual(saved["packet"], metadata)

    def test_udp_cli_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            # Let the OS select a free loopback port before the receiver starts.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            script = Path(__file__).resolve().parents[1] / "host" / "logger.py"
            process = subprocess.Popen(
                [sys.executable, str(script), "--bind", "127.0.0.1", "--port", str(port),
                 "--duration", "1", "--output", directory],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(timeout=5), "logger did not start")
                    self.assertIn("Listening on UDP", process.stdout.readline())
                metadata = {"v": 1, "type": "metadata", "node_id": 1, "boot_id": BOOT,
                            "role": "H1", "firmware_version": "0.1.0", "idf_version": "v5.4.2"}
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    for payload in (b"broken JSON", encoded(packet(0, 2)),
                                    encoded(metadata), encoded(packet(1, 2))):
                        sender.sendto(payload, ("127.0.0.1", port))
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr)
                self.assertIn("Saved 3 valid packets; rejected 1 malformed packets", stdout)
                runs = list(Path(directory).iterdir())
                self.assertEqual(len(runs), 1)
                rows = list(csv.DictReader(io.StringIO((runs[0] / "samples.csv").read_text())))
                self.assertEqual([row["seq"] for row in rows], ["0", "2", "1", "2"])
                self.assertEqual(rows[-1]["sequence_status"], "duplicate")
                self.assertTrue(all(int(row["receive_epoch_ns"]) > 0 for row in rows))
                self.assertTrue(all(row["source_ip"] == "127.0.0.1" for row in rows))
                self.assertEqual(len((runs[0] / "packets.jsonl").read_text().splitlines()), 3)
                self.assertEqual(len((runs[0] / "metadata.jsonl").read_text().splitlines()), 1)
                stats = json.loads((runs[0] / "stats.json").read_text())
                self.assertEqual(stats["invalid_packets"], 1)
                self.assertEqual(stats["streams"][0]["missing_samples"], 0)
                self.assertEqual(stats["streams"][0]["duplicate_samples"], 1)
                self.assertEqual(stats["streams"][0]["recovered_samples"], 1)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()

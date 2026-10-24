import copy
import csv
import io
import json
import tempfile
import unittest

from host.logger import ClockMapper, decode_packet, packet_identity, Recorder
from test_logger import BOOT, clock_document, encoded, packet


LEFT = "aa:bb:cc:dd:ee:01"
RIGHT = "aa:bb:cc:dd:ee:02"


def identified(wheel="front_left", device=LEFT, *sequences):
    result = packet(*sequences)
    result.update(v=2, wheel_id=wheel, device_id=device)
    return result


class WheelIdentityTests(unittest.TestCase):
    def test_v2_identity_is_required_and_canonical(self):
        original = identified()
        self.assertEqual(decode_packet(encoded(original)), original)
        self.assertEqual(packet_identity(original), ("front_left", LEFT, 1, BOOT))
        for key, value in (("wheel_id", "left"), ("wheel_id", None), ("wheel_id", []),
                           ("device_id", None), ("device_id", "AA:BB:CC:DD:EE:01"),
                           ("device_id", "aa:bb:cc:dd:ee:0g"),
                           ("device_id", "aabbccddee01"), ("device_id", "")):
            with self.subTest(key=key, value=value):
                invalid = copy.deepcopy(original)
                invalid[key] = value
                with self.assertRaises(ValueError):
                    decode_packet(encoded(invalid))
        for key in ("wheel_id", "device_id"):
            invalid = copy.deepcopy(original)
            del invalid[key]
            with self.assertRaises(ValueError):
                decode_packet(encoded(invalid))
        original["v"] = 1
        with self.assertRaises(ValueError):
            decode_packet(encoded(original))
        self.assertEqual(packet_identity(packet()), ("legacy", "", 1, BOOT))

    def test_same_role_and_boot_on_both_wheels_and_reboot_are_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            payloads = [identified("front_left", LEFT, 0, 2),
                        identified("front_right", RIGHT, 0),
                        identified("front_left", LEFT, 1),
                        identified("front_right", RIGHT, 1)]
            reboot = identified("front_left", LEFT, 0)
            reboot["boot_id"] = "fedcba9876543210"
            payloads.append(reboot)
            for payload in payloads:
                self.assertTrue(recorder.receive(encoded(payload), ("10.0.0.1", 9999), 1, 2))
            recorder.close()
            rows = list(csv.DictReader(io.StringIO((recorder.path / "samples.csv").read_text())))
            self.assertEqual(len(rows), 6)
            self.assertEqual(rows[2]["wheel_id"], "front_right")
            self.assertEqual(rows[2]["device_id"], RIGHT)
            self.assertEqual(rows[3]["sequence_status"], "recovered")
            self.assertTrue(all(row["sequence_status"] != "duplicate" for row in rows))
            stats = json.loads((recorder.path / "stats.json").read_text())
            self.assertEqual(len(stats["streams"]), 3)
            self.assertTrue(all(stream["missing_samples"] == 0 for stream in stats["streams"]))
            self.assertEqual(len(stats["assignments"]), 2)

    def test_conflicting_device_or_assignment_cannot_mix_with_original(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            self.assertTrue(recorder.receive(encoded(identified()), ("10.0.0.1", 1), 1, 2))
            self.assertFalse(recorder.receive(encoded(identified("front_left", RIGHT)), ("10.0.0.2", 2), 3, 4))
            self.assertFalse(recorder.receive(encoded(identified("front_right", LEFT)), ("10.0.0.1", 1), 3, 4))
            wrong_role = identified()
            wrong_role["node_id"] = 2
            self.assertFalse(recorder.receive(encoded(wrong_role), ("10.0.0.1", 1), 3, 4))
            recorder.close()
            self.assertEqual(recorder.identity_conflicts, 3)
            self.assertEqual(recorder.valid_packets, 1)
            self.assertEqual(len((recorder.path / "packets.jsonl").read_text().splitlines()), 1)

    def test_wheel_filter_rejects_other_wheel_and_legacy(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory, wheel="front_right")
            for payload in (identified(), packet()):
                self.assertFalse(recorder.receive(encoded(payload), ("127.0.0.1", 1), 1, 2))
            self.assertTrue(recorder.receive(encoded(identified("front_right", RIGHT)), ("127.0.0.1", 1), 1, 2))
            recorder.close()
            self.assertEqual(recorder.filtered_packets, 2)
            self.assertEqual(recorder.invalid_packets, 0)

    def test_clock_map_is_bound_to_wheel_device_role_and_boot(self):
        document = clock_document()
        legacy = copy.deepcopy(document["mappings"][0])
        document["mappings"][0].update(wheel_id="front_left", device_id=LEFT)
        right = copy.deepcopy(document["mappings"][0])
        right.update(wheel_id="front_right", device_id=RIGHT, offset_ns=10)
        document["mappings"].extend([right, legacy])
        mapper = ClockMapper(document)
        self.assertEqual(mapper.map(1, BOOT, 250000, "front_left", LEFT), 1791480000373488039)
        self.assertEqual(mapper.map(1, BOOT, 250000, "front_right", RIGHT), 250031260)
        self.assertIsNone(mapper.map(1, BOOT, 250000, "front_right", LEFT))
        self.assertIsNone(mapper.map(1, BOOT, 250000, "front_left", RIGHT))
        self.assertIsNone(mapper.map(1, "fedcba9876543210", 250000, "front_left", LEFT))
        self.assertEqual(mapper.map(1, BOOT, 250000), 1791480000373488039)
        for wheel, device in (("front_left", None), (None, LEFT), ("legacy", LEFT)):
            with self.assertRaises(ValueError):
                mapper.map(1, BOOT, 250000, wheel, device)
        del document["mappings"][0]["device_id"]
        with self.assertRaises(ValueError):
            ClockMapper(document)

    def test_v2_metadata_claims_identity_before_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = Recorder(directory)
            metadata = identified()
            metadata.update(type="metadata", role="H1", firmware_version="0.2.0", idf_version="v5.4.2")
            del metadata["samples"]
            del metadata["diagnostics"]
            self.assertTrue(recorder.receive(encoded(metadata), ("127.0.0.1", 1), 1, 2))
            self.assertFalse(recorder.receive(encoded(identified("front_left", RIGHT)), ("127.0.0.1", 2), 3, 4))
            recorder.close()
            saved = json.loads((recorder.path / "metadata.jsonl").read_text())["packet"]
            self.assertEqual(saved["device_id"], LEFT)
            self.assertEqual(saved["wheel_id"], "front_left")


if __name__ == "__main__":
    unittest.main()

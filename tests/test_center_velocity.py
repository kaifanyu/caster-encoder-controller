import contextlib
import csv
import io
import math
from pathlib import Path
import tempfile
import unittest

from host.center_velocity import estimate_rows, main, REQUIRED_FIELDS


BOOT = "0123456789abcdef"
OTHER_BOOT = "fedcba9876543210"


def sample(sequence, timestamp, position, *, boot=BOOT, flags=0x103, node=3, **extra):
    row = dict(node_id=str(node), boot_id=boot, seq=str(sequence), t_us=str(timestamp),
               position14=str(position) if position is not None else "", flags=str(flags))
    row.update(extra)
    return row


class CenterVelocityTests(unittest.TestCase):
    def assert_velocity(self, row, delta, interval_us, sign=1):
        self.assertEqual(row["status"], "estimated")
        self.assertEqual(row["interval_us"], interval_us)
        self.assertAlmostEqual(row["estimated_velocity_rad_s"],
                               sign * delta * math.tau * 1e6 / (16384 * interval_us))

    def test_forward_and_reverse_boundary_wrap_and_provisional_sign(self):
        rows = [sample(0, 1000, 16380), sample(1, 6000, 4), sample(2, 11000, 16380)]
        results = list(estimate_rows(rows))
        self.assertEqual(results[0]["status"], "first_sample")
        self.assertEqual(results[0]["estimated_velocity_rad_s"], "")
        self.assert_velocity(results[1], 8, 5000)
        self.assert_velocity(results[2], -8, 5000)
        reversed_results = list(estimate_rows(rows, sign=-1))
        self.assert_velocity(reversed_results[1], 8, 5000, sign=-1)
        self.assertEqual(reversed_results[1]["provisional_sign"], -1)
        self.assertEqual(results[1]["flags"], str(0x103))

    def test_skipped_sequence_uses_actual_timestamps(self):
        results = list(estimate_rows([sample(0, 1000, 100), sample(4, 21000, 140)]))
        self.assert_velocity(results[1], 40, 20000)

    def test_invalid_samples_never_become_endpoints(self):
        for flags in (1, 2, 3 | (1 << 7), 3 | (1 << 12)):
            with self.subTest(flags=flags):
                rows = [sample(0, 1000, 100), sample(1, 6000, 999, flags=flags),
                        sample(2, 11000, 120)]
                results = list(estimate_rows(rows))
                self.assertEqual(results[1]["status"], "invalid_sample")
                self.assertEqual(results[1]["estimated_velocity_rad_s"], "")
                self.assert_velocity(results[2], 20, 10000)
        results = list(estimate_rows([sample(0, 1000, None), sample(1, 6000, 100)]))
        self.assertEqual(results[1]["status"], "first_sample")

    def test_duplicate_reordered_and_nonmonotonic_rows_leave_history_intact(self):
        rows = [sample(0, 1000, 100), sample(2, 11000, 120),
                sample(2, 11000, 999), sample(1, 6000, 110),
                sample(3, 11000, 999), sample(4, 16000, 130)]
        results = list(estimate_rows(rows))
        self.assertEqual([row["status"] for row in results[2:5]],
                         ["duplicate", "reordered", "nonmonotonic_time"])
        self.assert_velocity(results[5], 10, 5000)

    def test_boots_have_separate_endpoints(self):
        rows = [sample(0, 1000, 100), sample(1, 6000, 110),
                sample(0, 1000, 8000, boot=OTHER_BOOT),
                sample(2, 11000, 120), sample(1, 6000, 8005, boot=OTHER_BOOT)]
        results = list(estimate_rows(rows))
        self.assertEqual(results[2]["status"], "first_sample")
        self.assertEqual(results[2]["estimated_velocity_rad_s"], "")
        self.assert_velocity(results[3], 10, 5000)
        self.assert_velocity(results[4], 5, 5000)

    def test_gap_rejection_then_local_recovery_including_invalid_burst(self):
        rows = [sample(0, 1000, 100), sample(1, 21000, None, flags=1 | (1 << 7)),
                sample(6, 31000, 200), sample(7, 36000, 210)]
        results = list(estimate_rows(rows))
        self.assertEqual(results[2]["status"], "gap_too_long")
        self.assertEqual(results[2]["interval_us"], 30000)
        self.assertEqual(results[2]["estimated_velocity_rad_s"], "")
        self.assert_velocity(results[3], 10, 5000)
        boundary = list(estimate_rows([sample(0, 1000, 100), sample(1, 26000, 120)]))
        self.assert_velocity(boundary[1], 20, 25000)

    def test_exact_half_turn_is_ambiguous_and_next_interval_can_recover(self):
        rows = [sample(0, 1000, 0), sample(1, 6000, 8192), sample(2, 11000, 8197)]
        results = list(estimate_rows(rows))
        self.assertEqual(results[1]["status"], "half_turn_ambiguous")
        self.assertEqual(results[1]["estimated_velocity_rad_s"], "")
        self.assert_velocity(results[2], 5, 5000)
        backwards = list(estimate_rows([sample(0, 1000, 8192), sample(1, 6000, 0)]))
        self.assertEqual(backwards[1]["status"], "half_turn_ambiguous")

    def test_non_center_rows_are_ignored(self):
        rows = [sample(0, 1000, 100), sample(99, 7000, 999, node=1),
                sample(99, 8000, 999, node=2), sample(1, 11000, 110)]
        results = list(estimate_rows(rows))
        self.assertEqual(len(results), 2)
        self.assert_velocity(results[1], 10, 10000)

    def test_same_boot_on_both_wheels_never_shares_history(self):
        left = dict(wheel_id="front_left", device_id="aa:bb:cc:dd:ee:01")
        right = dict(wheel_id="front_right", device_id="aa:bb:cc:dd:ee:02")
        rows = [sample(0, 1000, 100, **left), sample(0, 1000, 8000, **right),
                sample(1, 6000, 110, **left), sample(1, 6000, 7995, **right)]
        results = list(estimate_rows(rows))
        self.assertEqual([r["status"] for r in results[:2]], ["first_sample"] * 2)
        self.assert_velocity(results[2], 10, 5000)
        self.assert_velocity(results[3], -5, 5000)
        self.assertEqual(results[2]["wheel_id"], "front_left")
        self.assertEqual(results[3]["device_id"], right["device_id"])
        selected = list(estimate_rows(rows + [sample(2, 11000, 120)], wheel="front_right", sign=-1))
        self.assertEqual(len(selected), 2)
        self.assert_velocity(selected[1], -5, 5000, sign=-1)

    def test_identity_conflicts_and_partial_identity_do_not_change_endpoints(self):
        left = dict(wheel_id="front_left", device_id="aa:bb:cc:dd:ee:01")
        rows = [sample(0, 1000, 100, **left),
                sample(1, 6000, 9000, wheel_id="front_left", device_id="aa:bb:cc:dd:ee:02"),
                sample(1, 6000, 9000, wheel_id="front_right", device_id=left["device_id"]),
                sample(1, 6000, 9000, wheel_id="front_left"),
                sample(2, 11000, 120, **left)]
        results = list(estimate_rows(rows))
        self.assertEqual([r["status"] for r in results[1:4]],
                         ["identity_conflict", "identity_conflict", "invalid_identity"])
        self.assert_velocity(results[-1], 20, 10000)

    def test_cli_writes_adjacent_csv_and_explains_estimate(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "samples.csv"
            with source.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=REQUIRED_FIELDS)
                writer.writeheader()
                writer.writerows([sample(0, 1000, 100), sample(1, 6000, 110)])
            summary = io.StringIO()
            with contextlib.redirect_stdout(summary):
                self.assertEqual(main([str(source)]), 0)
            with source.with_name("center_velocity.csv").open(newline="", encoding="utf-8") as output:
                rows = list(csv.DictReader(output))
            self.assertEqual(rows[1]["status"], "estimated")
            self.assertEqual(rows[1]["seq"], "1")
            self.assertEqual(rows[1]["flags"], str(0x103))
            self.assertIn("less than half a turn", summary.getvalue())
            self.assertIn("sign is provisional", summary.getvalue())


if __name__ == "__main__":
    unittest.main()

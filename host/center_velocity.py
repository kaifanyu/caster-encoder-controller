#!/usr/bin/env python3
"""Estimate CENTER velocity offline from a logger samples.csv recording.

Each estimate assumes less than half a turn between its accepted endpoints.
The gap limit and provisional sign do not establish physical calibration or a
verified rate bound. Firmware flags are preserved unchanged in the output.
"""

import argparse
from collections import Counter
import csv
import math
from pathlib import Path


REQUIRED_FIELDS = ("node_id", "boot_id", "seq", "t_us", "position14", "flags")
OUTPUT_FIELDS = (*REQUIRED_FIELDS, "estimated_velocity_rad_s", "interval_us",
                 "status", "provisional_sign")
READY_AND_VALID = (1 << 0) | (1 << 1)
SPI_ERRORS = (1 << 7) | (1 << 12)
BINS = 16384


def estimate_rows(rows, *, max_gap_ms=25.0, sign=1):
    """Yield CENTER rows in arrival order, without joining different boots.

    Invalid samples and duplicate/reordered rows never become endpoints. The
    last good endpoint survives them, subject to the actual elapsed-time guard.
    A long gap or half-turn ambiguity starts a fresh local interval, allowing
    the next good pair to produce an estimate without recovering lost turns.
    """
    if not math.isfinite(max_gap_ms) or max_gap_ms <= 0:
        raise ValueError("max_gap_ms must be finite and positive")
    if sign not in (-1, 1):
        raise ValueError("sign must be -1 or 1")
    previous = {}
    highest_sequence = {}
    for row in rows:
        if row["node_id"] != "3":
            continue
        result = {name: row.get(name, "") for name in REQUIRED_FIELDS}
        result.update(estimated_velocity_rad_s="", interval_us="",
                      status="invalid_sample", provisional_sign=sign)
        try:
            boot = row["boot_id"]
            sequence = int(row["seq"])
            timestamp = int(row["t_us"])
            flags_text = row["flags"]
            flags = int(flags_text, 16 if flags_text.lower().startswith("0x") else 10)
            if not boot or sequence < 0 or timestamp < 0 or flags < 0:
                raise ValueError("invalid sample identity or flags")
        except (TypeError, ValueError, AttributeError):
            yield result
            continue
        highest = highest_sequence.get(boot, -1)
        if sequence <= highest or row.get("sequence_status") == "duplicate":
            result["status"] = "duplicate" if sequence == highest or row.get("sequence_status") == "duplicate" else "reordered"
            yield result
            continue
        highest_sequence[boot] = sequence
        try:
            position = int(row["position14"])
            if not 0 <= position < BINS:
                raise ValueError("position14 out of range")
        except (TypeError, ValueError):
            yield result
            continue
        if flags & READY_AND_VALID != READY_AND_VALID or flags & SPI_ERRORS:
            yield result
            continue
        before = previous.get(boot)
        if before is None:
            result["status"] = "first_sample"
        else:
            previous_time, previous_position = before
            interval_us = timestamp - previous_time
            result["interval_us"] = interval_us
            if interval_us <= 0:
                result["status"] = "nonmonotonic_time"
                yield result
                continue
            delta = position - previous_position
            if interval_us / 1000 > max_gap_ms:
                result["status"] = "gap_too_long"
            elif abs(delta) == BINS // 2:
                result["status"] = "half_turn_ambiguous"
            else:
                if delta > BINS // 2:
                    delta -= BINS
                elif delta < -BINS // 2:
                    delta += BINS
                result["estimated_velocity_rad_s"] = sign * delta * math.tau * 1e6 / (BINS * interval_us)
                result["status"] = "estimated"
        previous[boot] = (timestamp, position)
        yield result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path, help="existing logger samples.csv")
    parser.add_argument("--output", type=Path, help="default: adjacent center_velocity.csv")
    parser.add_argument("--max-gap-ms", type=float, default=25.0,
                        help="maximum endpoint interval; not a verified physical rate bound (default: 25)")
    parser.add_argument("--sign", type=int, choices=(-1, 1), default=1,
                        help="provisional direction sign (default: 1)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.max_gap_ms) or args.max_gap_ms <= 0:
        parser.error("--max-gap-ms must be finite and positive")
    destination = args.output or args.samples.with_name("center_velocity.csv")
    if destination.resolve() == args.samples.resolve():
        parser.error("--output must differ from the input recording")
    counts = Counter()
    try:
        with args.samples.open(newline="", encoding="utf-8-sig") as source:
            reader = csv.DictReader(source)
            missing = set(REQUIRED_FIELDS) - set(reader.fieldnames or ())
            if missing:
                parser.error("input CSV missing columns: " + ", ".join(sorted(missing)))
            with destination.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=OUTPUT_FIELDS)
                writer.writeheader()
                for row in estimate_rows(reader, max_gap_ms=args.max_gap_ms, sign=args.sign):
                    writer.writerow(row)
                    counts[row["status"]] += 1
    except OSError as error:
        parser.exit(1, f"Error: {error}\n")
    print(f"Saved {counts['estimated']} bench velocity estimates from {sum(counts.values())} CENTER rows to {destination}")
    print("Assumes less than half a turn between accepted endpoints; sign is provisional.")
    print(f"The {args.max_gap_ms:g} ms gap guard does not verify a physical speed bound; firmware flags are unchanged.")
    print("Statuses: " + ", ".join(f"{name}={count}" for name, count in sorted(counts.items())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

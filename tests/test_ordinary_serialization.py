"""The real serializer and binary encoder must agree with their published contract."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[1]
CJSON = os.environ.get("CJSON_SOURCE_DIRECTORY")


@unittest.skipUnless(CJSON, "requires the pinned SDK cJSON source")
class OrdinarySerializationTests(unittest.TestCase):
    def test_real_wire_and_flash_formats_for_both_wheels(self):
        self.check_roles(4)

    def test_real_split_board_wire_and_flash_formats(self):
        for node in (5, 6):
            self.check_roles(node)

    def check_roles(self, node):
        from ordinary_caster.protocol import decode_packet
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            obj = root / "cJSON.o"
            subprocess.run(["gcc", "-std=c99", "-c", str(Path(CJSON) / "cJSON.c"), "-o", str(obj)], check=True)
            for index, wheel in enumerate(("front_left", "front_right"), 1):
                binary = root / wheel
                log = root / f"{wheel}.bin"
                subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(REPO / "main"), "-I", str(REPO / "ordinary_caster/config"),
                    "-I", str(REPO / "tests/stubs"), "-I", CJSON,
                    f"-DCASTER_NODE_ID={node}", f"-DCASTER_WHEEL_INDEX={index}",
                    f'-DCASTER_CONFIG_HEADER="{wheel}.example.h"',
                    str(REPO / "tests/test_ordinary_serialization.cpp"), str(REPO / "main/protocol.cpp"),
                    str(obj), "-o", str(binary)], check=True, capture_output=True)
                datagrams = subprocess.check_output([str(binary), str(log)]).splitlines()
                packets = [decode_packet(value) for value in datagrams]
                self.assertEqual(len(packets), 4)
                for packet, datagram in zip(packets, datagrams):
                    self.assertEqual((packet["v"], packet["caster_type"], packet["wheel_id"], packet["node_id"]),
                                     (3 if node == 4 else 4, "ordinary", wheel, node))
                    self.assertLessEqual(len(datagram), 1472)
                metadata = packets[-1]
                self.assertEqual(metadata["udp_port"], 5006 + index)
                active = ("swivel", "wheel") if node == 4 else ("swivel",) if node == 5 else ("wheel",)
                self.assertEqual(set(metadata) & {"swivel", "wheel"}, set(active))
                if "swivel" in active:
                    self.assertEqual(metadata["swivel"]["resolution_bits"], 12)
                if "wheel" in active:
                    self.assertEqual(metadata["wheel"]["counts_per_rev"], 128)
                    self.assertFalse(metadata["wheel"]["cpr_verified"])
                data = log.read_bytes()
                self.assertEqual(data[:8], b"CASTLOG2")
                version, size, length, crc = struct.unpack_from("<4I", data, 8)
                self.assertEqual((version, size), (2, 116))
                self.assertEqual(zlib.crc32(data[24:24+length]), crc)
                self.assertEqual(json.loads(data[24:24+length])["packet"], metadata)
                record = struct.Struct("<QqqqHhIII8dI")
                for i, packet in enumerate(packets[:3]):
                    raw = data[4096+i*116:4096+(i+1)*116]
                    values = record.unpack(raw)
                    sample = packet["samples"][0]
                    self.assertEqual(values[0], sample["seq"])
                    self.assertEqual(values[1], sample["t_us"])
                    self.assertEqual(set(sample) & {"swivel", "wheel"}, set(active))
                    if "wheel" in active:
                        self.assertEqual(values[2], sample["wheel"]["t_us"])
                        self.assertEqual(values[3], -(1 << 63) + i)
                    else:
                        self.assertEqual(values[2:4], (0, 0))
                    self.assertEqual(values[6:8], (sample.get("swivel", {}).get("flags", 0), sample.get("wheel", {}).get("flags", 0)))
                    self.assertEqual(zlib.crc32(raw[:-4]), values[-1])
                if "swivel" in active:
                    self.assertIsNone(packets[1]["samples"][0]["swivel"]["position12"])
                if "wheel" in active:
                    self.assertEqual(packets[1]["samples"][0]["wheel"]["raw_count"], str(-(1 << 63) + 1))
                    self.assertIsNone(packets[2]["samples"][0]["wheel"]["raw_count"])
                # Optional exact C++ fixtures for the companion ROS decoder tests.
                if os.environ.get("ORDINARY_CONTRACT_OUTPUT"):
                    target = Path(os.environ["ORDINARY_CONTRACT_OUTPUT"])
                    target.mkdir(parents=True, exist_ok=True)
                    name = wheel if node == 4 else wheel + ("_swivel" if node == 5 else "_wheel")
                    (target / f"{name}.bin").write_bytes(data)
                    (target / f"{name}.jsonl").write_bytes(b"\n".join(datagrams) + b"\n")

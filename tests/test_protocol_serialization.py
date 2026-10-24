"""Run with CJSON_SOURCE_DIRECTORY pointing to the pinned ESP-IDF cJSON source."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from host.logger import decode_packet


REPO = Path(__file__).resolve().parents[1]
CJSON = os.environ.get("CJSON_SOURCE_DIRECTORY")


@unittest.skipUnless(CJSON, "run scripts/test_protocol.sh to validate real firmware serialization")
class FirmwareSerializationTests(unittest.TestCase):
    def test_all_six_firmware_profiles_decode_without_losing_identity_or_precision(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cjson = Path(CJSON)
            object_file = root / "cJSON.o"
            subprocess.run(["gcc", "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(object_file)],
                           check=True, capture_output=True, text=True)
            for index, wheel in enumerate(("front_left", "front_right"), 1):
                for node, role in enumerate(("H1", "H2", "CENTER"), 1):
                    with self.subTest(wheel=wheel, role=role):
                        binary = root / f"{wheel}-{role}"
                        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                        "-I", str(REPO / "main"), "-I", str(REPO / "config"),
                                        "-I", str(REPO / "tests" / "stubs"), "-I", str(cjson),
                                        f"-DCASTER_NODE_ID={node}", f"-DCASTER_WHEEL_INDEX={index}",
                                        f'-DCASTER_CONFIG_HEADER="{wheel}.example.h"',
                                        str(REPO / "tests" / "test_protocol_serialization.cpp"),
                                        str(REPO / "main" / "protocol.cpp"), str(object_file),
                                        "-o", str(binary)], check=True, capture_output=True, text=True)
                        datagrams = subprocess.check_output([str(binary)]).splitlines()
                        packets = [decode_packet(data) for data in datagrams]
                        self.assertEqual(len(packets), 2)
                        for packet in packets:
                            self.assertEqual(packet["v"], 2)
                            self.assertEqual(packet["wheel_id"], wheel)
                            self.assertEqual(packet["node_id"], node)
                            self.assertEqual(packet["device_id"], "aa:bb:cc:dd:ee:01")
                        self.assertEqual(packets[1]["role"], role)
                        self.assertEqual(packets[1]["receiver_ipv4"], "192.168.1.100")
                        self.assertEqual(packets[1]["udp_port"], 5004 + index)
                        self.assertEqual(packets[1]["configured_ipv4"], "")
                        self.assertEqual(packets[1]["network_gateway"], "192.168.1.1")
                        self.assertEqual(packets[1]["network_netmask"], "255.255.255.0")
                        self.assertNotIn("WIFI_PASSWORD", packets[1])
                        samples = packets[0]["samples"]
                        self.assertEqual(samples[1]["seq"], (1 << 64) - 1)
                        self.assertEqual(samples[1]["t_us"], (1 << 63) - 1)
                        self.assertEqual(samples[1]["angle_rad"], None)
                        if node != 3:
                            self.assertEqual(samples[1]["raw_count"], str(-(1 << 63)))
                        # Two-sample worst-case telemetry fits standard Ethernet IPv4 MTU.
                        for data in datagrams:
                            self.assertLessEqual(len(data), 1472)


if __name__ == "__main__":
    unittest.main()

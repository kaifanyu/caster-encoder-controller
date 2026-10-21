#!/usr/bin/env python3
"""Forward encoder UDP datagrams unchanged from Windows to a WSL receiver.

The receiver sees the relay's source IP. Encoder acquisition t_us, node_id,
boot_id, and every other payload byte remain unchanged.
"""

import argparse
import ipaddress
import math
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-ip", required=True, type=ipaddress.IPv4Address)
    parser.add_argument("--listen-port", type=int, default=5005)
    parser.add_argument("--target-port", type=int, default=5005)
    parser.add_argument("--duration", type=float)
    args = parser.parse_args()
    if not all(1 <= port <= 65535 for port in (args.listen_port, args.target_port)):
        parser.error("ports must be in 1..65535")
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be finite and positive")
    if args.target_ip.is_unspecified or args.target_ip.is_multicast or str(args.target_ip) == "255.255.255.255":
        parser.error("--target-ip must be a unicast destination")
    target = (str(args.target_ip), args.target_port)
    local_ips = {entry[4][0] for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    if args.target_port == args.listen_port and (args.target_ip.is_loopback or target[0] in local_ips):
        parser.error("target is this Windows listener; choose a WSL IP or a different target port")

    forwarded = errors = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        relay.bind(("0.0.0.0", args.listen_port))
        print(f"Relaying UDP 0.0.0.0:{args.listen_port} -> {target[0]}:{target[1]}; payload unchanged.", flush=True)
        print("Receiver source IP is the Windows relay; encoder timestamps and identity are retained.", flush=True)
        started = last_report = time.monotonic()
        try:
            while True:
                now = time.monotonic()
                remaining = None if args.duration is None else args.duration - (now - started)
                if remaining is not None and remaining <= 0:
                    break
                relay.settimeout(min(0.25, remaining) if remaining is not None else 0.25)
                try:
                    data, source = relay.recvfrom(65535)
                    # Prevent a loop even if hostname lookup omitted a local adapter.
                    if args.target_port == args.listen_port and source == target:
                        raise RuntimeError("received our own relay packet; target is a local Windows address")
                    relay.sendto(data, target)
                    forwarded += 1
                except socket.timeout:
                    pass
                except OSError:
                    errors += 1
                now = time.monotonic()
                if now - last_report >= 1:
                    print(f"forwarded={forwarded} errors={errors}", flush=True)
                    last_report = now
        except KeyboardInterrupt:
            pass
        finally:
            print(f"Relay stopped: forwarded={forwarded} errors={errors}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

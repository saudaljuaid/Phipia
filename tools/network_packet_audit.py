#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audit packet-level proof produced by the deterministic network fixture."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("525400654321")


def frames(path: Path):
    with path.open("rb") as stream:
        header = stream.read(24)
        if len(header) != 24 or struct.unpack_from("<I", header)[0] != 0xA1B2C3D4:
            raise ValueError("not a little-endian Ethernet PCAP")
        if struct.unpack_from("<I", header, 20)[0] != 1:
            raise ValueError("capture is not Ethernet")
        while True:
            record = stream.read(16)
            if not record:
                return
            if len(record) != 16:
                raise ValueError("truncated PCAP record")
            captured, original = struct.unpack_from("<II", record, 8)
            if captured != original or captured > 1514:
                raise ValueError("invalid captured Ethernet length")
            frame = stream.read(captured)
            if len(frame) != captured:
                raise ValueError("truncated Ethernet frame")
            yield frame


def audit(path: Path) -> dict[str, object]:
    counts = {name: 0 for name in
              ("guest_tx", "peer_tx", "arp", "ipv4", "icmp", "udp",
               "dhcp", "dns", "tcp", "http")}
    malformed = 0
    for frame in frames(path):
        if len(frame) < 14:
            malformed += 1
            continue
        if frame[6:12] == GUEST_MAC:
            counts["guest_tx"] += 1
        elif frame[6:12] == PEER_MAC:
            counts["peer_tx"] += 1
        kind = struct.unpack_from("!H", frame, 12)[0]
        if kind == 0x0806:
            counts["arp"] += 1
            continue
        if kind != 0x0800 or len(frame) < 34:
            continue
        packet = frame[14:]
        header_length = (packet[0] & 15) * 4
        total = struct.unpack_from("!H", packet, 2)[0]
        if packet[0] >> 4 != 4 or header_length < 20 or total > len(packet):
            malformed += 1
            continue
        counts["ipv4"] += 1
        protocol = packet[9]
        payload = packet[header_length:total]
        if protocol == 1:
            counts["icmp"] += 1
        elif protocol == 17 and len(payload) >= 8:
            counts["udp"] += 1
            source, destination, length = struct.unpack_from("!HHH", payload)
            if length <= len(payload):
                if {source, destination} == {67, 68}:
                    counts["dhcp"] += 1
                if source == 53 or destination == 53:
                    counts["dns"] += 1
        elif protocol == 6 and len(payload) >= 20:
            counts["tcp"] += 1
            offset = (payload[12] >> 4) * 4
            if offset <= len(payload) and (b"HTTP/1.1" in payload[offset:] or
                                           b"GET /" in payload[offset:] or
                                           b"HEAD /" in payload[offset:]):
                counts["http"] += 1
    required = ("guest_tx", "peer_tx", "arp", "ipv4", "icmp", "udp",
                "dhcp", "dns", "tcp", "http")
    missing = [name for name in required if counts[name] == 0]
    return {"pcap": str(path), "counts": counts, "malformed": malformed,
            "required": list(required), "missing": missing,
            "production_path": not missing}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        result = audit(args.capture)
    except (OSError, ValueError) as error:
        print(f"packet audit: {error}", file=sys.stderr)
        return 2
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["production_path"] else 1


if __name__ == "__main__":
    sys.exit(main())

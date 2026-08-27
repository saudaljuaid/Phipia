#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""An independent record of the reference NVIDIA VBIOS image.

This file is written from the specifications rather than from the kernel: the
PCI Firmware Specification 3.0 section 5.1 for the expansion ROM header and the
PCIR data structure, and the BIT table layout the Nouveau driver reads and the
envytools project documents. It exists so the same 1,024 bytes are stated three
independent times -- here, in the kernel's C table, and in the freestanding
Rust validator -- and any two of them disagreeing is a build failure rather
than a parser that quietly accepts something else.

The image is synthesised, not dumped. No board's ROM is reproduced here. Its
PCIR device identifier is 0x5341 precisely so it can never be mistaken for a
real part.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

IMAGE_BYTES = 1024
BLOCK_BYTES = 512
NVIDIA_VENDOR_ID = 0x10DE
REFERENCE_DEVICE_ID = 0x5341
PCIR_OFFSET = 0x40
BIT_OFFSET = 0x100
BIT_TOKEN_BYTES = 6
BIT_SIGNATURE = bytes((0xFF, 0xB8)) + b"BIT\x00"

# Each entry is (identifier, region length, region offset). The identifiers are
# the ones Nouveau looks up by name; the regions are inside the image, which is
# the only property the validator enforces.
BIT_TOKENS = (
    (ord("i"), 0x0040, 0x0200),
    (ord("B"), 0x0020, 0x0240),
    (ord("P"), 0x0010, 0x0260),
)

IMAGE_SHA256 = "982C38A511FEEF8BEA2BC08EAA847C77C36EA4CD00D9F4AB508911E20E76E975"


def image() -> bytes:
    """Build the reference image from the documented field layout."""
    data = bytearray(IMAGE_BYTES)

    # PCI Firmware Specification 3.0, table 5-1: the expansion ROM header.
    data[0] = 0x55
    data[1] = 0xAA
    data[2] = IMAGE_BYTES // BLOCK_BYTES
    # A short jump and a pad, standing in for the initialisation entry point.
    data[3] = 0xEB
    data[4] = 0x0A
    data[5] = 0x90
    data[0x18] = PCIR_OFFSET & 0xFF
    data[0x19] = (PCIR_OFFSET >> 8) & 0xFF

    # The same specification, table 5-2: the PCI data structure.
    data[PCIR_OFFSET:PCIR_OFFSET + 4] = b"PCIR"
    data[PCIR_OFFSET + 4] = NVIDIA_VENDOR_ID & 0xFF
    data[PCIR_OFFSET + 5] = (NVIDIA_VENDOR_ID >> 8) & 0xFF
    data[PCIR_OFFSET + 6] = REFERENCE_DEVICE_ID & 0xFF
    data[PCIR_OFFSET + 7] = (REFERENCE_DEVICE_ID >> 8) & 0xFF
    data[PCIR_OFFSET + 0x0A] = 0x18
    data[PCIR_OFFSET + 0x0B] = 0x00
    # Class code, three bytes, interface first: a VGA display controller.
    data[PCIR_OFFSET + 0x0D] = 0x00
    data[PCIR_OFFSET + 0x0E] = 0x00
    data[PCIR_OFFSET + 0x0F] = 0x03
    data[PCIR_OFFSET + 0x10] = IMAGE_BYTES // BLOCK_BYTES
    data[PCIR_OFFSET + 0x11] = 0x00
    # Code type 0x00 is x86 PC-AT; bit 7 of the indicator marks the last image.
    data[PCIR_OFFSET + 0x14] = 0x00
    data[PCIR_OFFSET + 0x15] = 0x80

    # The BIT table: identifier 0xB8FF little-endian, then "BIT\0".
    data[BIT_OFFSET:BIT_OFFSET + len(BIT_SIGNATURE)] = BIT_SIGNATURE
    data[BIT_OFFSET + 6] = 0x01
    data[BIT_OFFSET + 7] = 0x00
    data[BIT_OFFSET + 8] = 12
    data[BIT_OFFSET + 9] = BIT_TOKEN_BYTES
    data[BIT_OFFSET + 10] = len(BIT_TOKENS)
    data[BIT_OFFSET + 11] = 0x00
    for index, (identifier, length, offset) in enumerate(BIT_TOKENS):
        token = BIT_OFFSET + 12 + index * BIT_TOKEN_BYTES
        data[token] = identifier
        data[token + 1] = 0x02
        data[token + 2] = length & 0xFF
        data[token + 3] = (length >> 8) & 0xFF
        data[token + 4] = offset & 0xFF
        data[token + 5] = (offset >> 8) & 0xFF
    return bytes(data)


def digest() -> str:
    return hashlib.sha256(image()).hexdigest().upper()


def kernel_table(source: Path) -> bytes:
    """Extract the kernel's own copy of the same image."""
    text = source.read_text(encoding="utf-8")
    match = re.search(
        r"static const uint8_t reference_vbios\[NVIDIA_REFERENCE_VBIOS_BYTES\]"
        r"\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if match is None:
        raise SystemExit("the kernel reference VBIOS table was not found")
    values = re.findall(r"0[xX]([0-9A-Fa-f]{2})", match.group(1))
    return bytes(int(value, 16) for value in values)


def emit_c_table() -> str:
    data = image()
    lines = []
    for start in range(0, len(data), 12):
        chunk = data[start:start + 12]
        lines.append("    " + " ".join(f"0x{byte:02X}," for byte in chunk))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--digest", action="store_true")
    parser.add_argument("--emit-c", action="store_true")
    parser.add_argument("--write", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.emit_c:
        print(emit_c_table())
        return 0
    if args.write is not None:
        args.write.write_bytes(image())
        return 0
    if args.digest:
        print(digest())
        return 0
    if args.self_test:
        data = image()
        assert len(data) == IMAGE_BYTES
        assert data[0:2] == b"\x55\xAA"
        assert data[PCIR_OFFSET:PCIR_OFFSET + 4] == b"PCIR"
        assert int.from_bytes(
            data[PCIR_OFFSET + 4:PCIR_OFFSET + 6], "little"
        ) == NVIDIA_VENDOR_ID
        assert data[BIT_OFFSET:BIT_OFFSET + 6] == BIT_SIGNATURE
        assert digest() == IMAGE_SHA256, digest()
        print(f"NVIDIA reference VBIOS record: {IMAGE_BYTES} bytes, "
              f"SHA-256 {IMAGE_SHA256}")
        return 0
    print(digest())
    return 0


if __name__ == "__main__":
    sys.exit(main())

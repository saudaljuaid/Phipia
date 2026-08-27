#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hold the kernel's reference VBIOS table to the independent record."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nvidia_vbios_image as record  # noqa: E402


def main() -> int:
    source = Path("src/kernel/nvidia.c")
    expected = record.image()
    actual = record.kernel_table(source)

    if len(actual) != len(expected):
        print(
            f"kernel reference VBIOS table is {len(actual)} bytes, "
            f"the record is {len(expected)}",
            file=sys.stderr,
        )
        return 1
    for index, (left, right) in enumerate(zip(actual, expected)):
        if left != right:
            print(
                f"kernel reference VBIOS table differs at 0x{index:04X}: "
                f"kernel 0x{left:02X}, record 0x{right:02X}",
                file=sys.stderr,
            )
            return 1
    print(
        f"NVIDIA reference VBIOS image: {len(expected)} bytes agreed by the "
        f"kernel table and the independent record, SHA-256 {record.digest()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

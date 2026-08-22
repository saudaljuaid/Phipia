#!/usr/bin/env python3
"""Create the bounded, deterministic, read-only-to-the-guest NVMe fixture."""

from __future__ import annotations

import argparse
from pathlib import Path


BLOCK_BYTES = 4096
BLOCK_COUNT = 16
FIXTURE_LBA = 8


def fixture_byte(index: int) -> int:
    return (index * 37 + 11) & 0xFF


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)

    image = bytearray(BLOCK_BYTES * BLOCK_COUNT)
    start = FIXTURE_LBA * BLOCK_BYTES
    image[start : start + BLOCK_BYTES] = bytes(
        fixture_byte(index) for index in range(BLOCK_BYTES)
    )
    output.write_bytes(image)


if __name__ == "__main__":
    main()

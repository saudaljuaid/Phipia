#!/usr/bin/env python3
"""Prove the kernel's multiprocess executable table matches the record here."""

from __future__ import annotations

from pathlib import Path
import sys

from multiprocess_image import (
    FILE_BYTES,
    IMAGE_SHA256,
    build_payload,
    kernel_table,
    verify_payload,
)


def main() -> int:
    repository = Path(__file__).resolve().parents[1]
    source = repository / "src" / "kernel" / "multiprocess.c"
    try:
        payload = build_payload()
        verify_payload(payload)
        table = kernel_table(source)
        if len(table) != FILE_BYTES:
            raise ValueError(
                f"the kernel table holds {len(table)} bytes, not {FILE_BYTES}"
            )
        if table != payload:
            offset = next(
                index
                for index, (left, right) in enumerate(zip(table, payload))
                if left != right
            )
            raise ValueError(
                f"the kernel table differs from the record at byte {offset}"
            )
    except (OSError, StopIteration, ValueError) as error:
        print(f"multiprocess executable refused: {error}", file=sys.stderr)
        return 1
    print(
        f"{source}: multiprocess ELF64 ET_EXEC {FILE_BYTES} bytes, "
        f"SHA-256 {IMAGE_SHA256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

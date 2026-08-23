#!/usr/bin/env python3
"""Reject FP/MMX/SIMD instructions covered by QEMU translated blocks."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


DISASSEMBLY_LINE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+([a-zA-Z][a-zA-Z0-9.]*)\s*(.*)$"
)
QEMU_ADDRESS = re.compile(r"^0x([0-9a-fA-F]+):\s*$")
QEMU_BYTES = re.compile(r"^OBJD-T:\s*([0-9a-fA-F]+)\s*$")
VECTOR_REGISTER = re.compile(r"%(?:xmm|ymm|zmm|mm|k)[0-9]+", re.IGNORECASE)
FORBIDDEN_MNEMONIC = re.compile(
    r"^(?:f[a-zA-Z0-9.]*|emms|fxsave|fxrstor|ldmxcsr|stmxcsr|v[a-zA-Z0-9.]*)$",
    re.IGNORECASE,
)


def forbidden_instructions(text: str) -> list[tuple[int, str]]:
    found: list[tuple[int, str]] = []
    for line in text.splitlines():
        match = DISASSEMBLY_LINE.match(line)
        if match is None:
            continue
        address = int(match.group(1), 16)
        mnemonic = match.group(2).lower()
        operands = match.group(3)
        if mnemonic in {"verr", "verw"}:
            continue
        if VECTOR_REGISTER.search(operands) or FORBIDDEN_MNEMONIC.match(mnemonic):
            found.append((address, line.strip()))
    return found


def translated_ranges(text: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    start: int | None = None
    byte_count = 0

    def finish() -> None:
        nonlocal start, byte_count
        if start is not None and byte_count != 0:
            ranges.append((start, start + byte_count))
        start = None
        byte_count = 0

    for line in text.splitlines():
        address = QEMU_ADDRESS.match(line)
        if address is not None:
            finish()
            start = int(address.group(1), 16)
            continue
        encoded = QEMU_BYTES.match(line)
        if encoded is not None and start is not None:
            if len(encoded.group(1)) % 2 != 0:
                raise ValueError("QEMU translated bytes have odd length")
            byte_count += len(encoded.group(1)) // 2
    finish()
    if not ranges:
        raise ValueError("QEMU trace contains no translated instruction blocks")
    return ranges


def exercised_forbidden(disassembly: str, trace: str) -> list[str]:
    instructions = forbidden_instructions(disassembly)
    ranges = translated_ranges(trace)
    return [
        line
        for address, line in instructions
        if any(start <= address < end for start, end in ranges)
    ]


def self_test() -> None:
    disassembly = """
  4000010061fe: lea 0xa0(%rsp),%rcx
  400001006208: movups (%rdx),%xmm0
  40000100620b: movups %xmm0,0x10(%rsp)
  400001006210: mov 0x10(%rdx),%rax
  400001007000: verr %ax
"""
    trace = """
IN:
0x4000010061fe:
OBJD-T: 488d8c24a000000031ff0f10020f11442410
"""
    refused = exercised_forbidden(disassembly, trace)
    if len(refused) != 2 or "6208" not in refused[0] or "620b" not in refused[1]:
        raise AssertionError("translated-block overlap did not reject both SIMD instructions")
    clean = trace.replace("0x4000010061fe", "0x400001008000")
    if exercised_forbidden(disassembly, clean):
        raise AssertionError("an unexecuted SIMD instruction was rejected")
    try:
        translated_ranges("IN:\n0x4000:\n")
    except ValueError:
        pass
    else:
        raise AssertionError("an empty translated block was accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--disassembly", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.disassembly is None or args.trace is None:
        parser.error("--disassembly and --trace are required")
    try:
        refused = exercised_forbidden(
            args.disassembly.read_text(encoding="utf-8"),
            args.trace.read_text(encoding="utf-8"),
        )
    except (OSError, UnicodeError, ValueError) as error:
        print(f"instruction evidence is invalid: {error}", file=sys.stderr)
        return 1
    if refused:
        print("exercised text contains FP, MMX, SSE, or AVX instructions:", file=sys.stderr)
        for line in refused:
            print(line, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

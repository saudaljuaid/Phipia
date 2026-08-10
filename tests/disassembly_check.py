#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reject floating-point and vector instructions in kernel executable code."""

from __future__ import annotations

import argparse
import re
import subprocess


INSTRUCTION = re.compile(
    r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+)+([a-zA-Z0-9_.]+)(?:\s+(.*))?$"
)
VECTOR_REGISTER = re.compile(r"%(?:xmm|ymm|zmm|mm)[0-9]+|%st(?:\([0-7]\))?")
STANDALONE_VECTOR = {
    "emms",
    "femms",
    "ldmxcsr",
    "stmxcsr",
    "vzeroall",
    "vzeroupper",
    "wait",
    "xrstor",
    "xrstor64",
    "xrstors",
    "xrstors64",
    "xsave",
    "xsave64",
    "xsavec",
    "xsavec64",
    "xsaveopt",
    "xsaveopt64",
    "xsaves",
    "xsaves64",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("objdump")
    parser.add_argument("elf")
    arguments = parser.parse_args()
    completed = subprocess.run(
        [arguments.objdump, "-d", arguments.elf],
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        parser.error(completed.stderr.strip() or "objdump failed")

    rejected: list[str] = []
    for line in completed.stdout.splitlines():
        match = INSTRUCTION.match(line)
        if match is None:
            continue
        mnemonic = match.group(1).lower()
        operands = (match.group(2) or "").lower()
        if (
            mnemonic.startswith("f")
            or mnemonic in STANDALONE_VECTOR
            or VECTOR_REGISTER.search(operands) is not None
        ):
            rejected.append(line.strip())

    if rejected:
        print("floating-point or vector instructions found:")
        print("\n".join(rejected[:40]))
        return 1
    print("disassembly contains no x87, MMX, SSE, AVX, or vector registers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

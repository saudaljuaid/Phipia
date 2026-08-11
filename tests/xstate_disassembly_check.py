#!/usr/bin/env python3
"""Verify that the pure XSAVE metadata core stays freestanding and scalar."""

from __future__ import annotations

import argparse
import re
import subprocess


def run(tool: str, *arguments: str) -> str:
    result = subprocess.run(
        [tool, *arguments],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr or f"{tool} exited with {result.returncode}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("object")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()

    undefined = run(args.nm, "-u", args.object).strip()
    if undefined:
        print("XSAVE core unexpectedly depends on runtime helpers:")
        print(undefined)
        return 1

    disassembly = run(args.objdump, "-d", args.object).lower()
    forbidden = re.compile(
        r"\b(?:xsave(?:64|c|c64|opt|opt64|s|s64)?|"
        r"xrstor(?:64|s|s64)?|fxsave(?:64)?|fxrstor(?:64)?)\b|"
        r"%(?:xmm|ymm|zmm)[0-9]+"
    )
    match = forbidden.search(disassembly)
    if match is not None:
        print(f"unexpected architectural state instruction/register: {match.group(0)}")
        return 1

    print("XSAVE core disassembly passed: scalar and runtime-helper free")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

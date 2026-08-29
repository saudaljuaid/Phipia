#!/usr/bin/env python3
"""Host refusal and reproducibility tests for the Sapote package format."""

from __future__ import annotations

import copy
import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "sapote_package", ROOT / "tools" / "sapote-package.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load package tool")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


def expect_refusal(package: bytes) -> None:
    try:
        PACKAGE.parse_package(package)
    except PACKAGE.PackageError:
        return
    raise AssertionError("malformed package was accepted")


def main() -> int:
    executable = b"\x7fELF" + bytes(range(64))
    spec = {
        "name": "Package Test",
        "identifier": "PKGTEST",
        "executable": "PKGTEST.APP",
        "data_namespace": "PKGTEST",
        "memory_limit": 1024 * 1024,
        "max_handles": 32,
        "max_threads": 2,
        "capabilities": ["console", "data-read"],
        "arguments": ["PKGTEST.APP", "one"],
    }
    first = PACKAGE.build_package(spec, executable)
    second = PACKAGE.build_package(copy.deepcopy(spec), executable)
    assert first == second
    _, parsed_executable, report = PACKAGE.parse_package(first)
    assert parsed_executable == executable
    assert report["identifier"] == "PKGTEST"
    assert report["arguments"] == ["PKGTEST.APP", "one"]
    changed = bytearray(first)
    changed[-1] ^= 1
    expect_refusal(bytes(changed))
    changed = bytearray(first)
    changed[24] = 1
    expect_refusal(bytes(changed))
    print("Sapote package host tests passed: reproducible, digest, reserved, manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

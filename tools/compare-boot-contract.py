#!/usr/bin/env python3
"""Compare a pre-ledger normal transcript and the installed boot contract."""

from __future__ import annotations

import argparse
import difflib
import pathlib
import re
import sys


SCENARIOS = {
    "normal": ("0x10", 33),
    "breakpoint": ("0x11", 35),
    "invalid-opcode": ("0x12", 37),
    "page-fault": ("0x13", 39),
    "ist": ("0x14", 41),
    "pit": ("0x15", 43),
    "unexpected": ("0x16", 45),
    "double-fault": ("0x17", 47),
    "apic": ("0x18", 49),
    "ioapic": ("0x19", 51),
    "retired": ("0x1A", 53),
    "apic-timer": ("0x1B", 55),
    "tsc": ("0x1C", 57),
    "pm-timer": ("0x1D", 59),
    "pit-retired": ("0x1E", 61),
    "timers": ("0x1F", 63),
    "paging": ("0x20", 65),
    "heap": ("0x21", 67),
    "ioapic-level": ("0x22", 69),
    "pci": ("0x23", 71),
    "pci-ecam": ("0x24", 73),
    "threads": ("0x25", 75),
    "thread-guard": ("0x26", 77),
    "framebuffer": ("0x27", 79),
    "screen": ("0x28", 81),
    "keyboard": ("0x29", 83),
    "shell": ("0x2A", 85),
    "surface": ("0x2B", 87),
    "write-combining": ("0x2C", 89),
    "device-windows": ("0x2D", 91),
    "boot-ledger": ("0x2E", 93),
}

LEDGER_PROOF = "OpenSeneri: Boot Ledger installed proof passed"


def normalize(line: str) -> str:
    """Mask only measured, address, or linked-image-derived numeric fields."""

    if line.startswith((
        "OpenSeneri: allocatable frames:",
        "OpenSeneri: free frames:",
        "OpenSeneri: reserved frames:",
    )):
        return re.sub(r"[0-9]+$", "<image-count>", line)

    if line.startswith("OpenSeneri: frame probe:"):
        return re.sub(r"0x[0-9A-F]{16}", "<address>", line)

    if line.startswith("OpenSeneri: paging root "):
        return re.sub(r"0x[0-9A-F]{16}", "<address>", line, count=1)

    if line.startswith("OpenSeneri: paging leaves "):
        return re.sub(
            r"writable [0-9]+ executable [0-9]+",
            "writable <image-count> executable <image-count>",
            line,
        )

    if line.startswith("OpenSeneri: surface cycles "):
        return re.sub(r"[0-9]+", "<cycles>", line)

    if line.startswith("OpenSeneri: surface split cycles "):
        return re.sub(r"[0-9]+", "<cycles>", line)

    if line.startswith("OpenSeneri: surface sparse two-corner cycles "):
        line = re.sub(r"(total|draw|push) [0-9]+", r"\1 <cycles>", line)
        return line

    if line.startswith("OpenSeneri: I/O APIC level deliveries "):
        return re.sub(r"in [0-9]+ ns$", "in <time> ns", line)

    if line.startswith("OpenSeneri: PM timer counted "):
        return re.sub(r"in [0-9]+ ns$", "in <time> ns", line)

    if line.startswith("OpenSeneri: local APIC timer calibrated at "):
        return re.sub(r"at [0-9]+ counts", "at <rate> counts", line)

    if line.startswith("OpenSeneri: TSC calibrated at "):
        return re.sub(r"at [0-9]+ Hz", "at <rate> Hz", line)

    if line.startswith("OpenSeneri: clocks agree: "):
        return re.sub(r"[0-9]+ ns", "<time> ns", line)

    if line.startswith("OpenSeneri: slept "):
        return re.sub(r"slept [0-9]+ ns", "slept <time> ns", line)

    if line.startswith("OpenSeneri: preempted "):
        return re.sub(r"in [0-9]+ ms$", "in <time> ms", line)

    if line.startswith("OpenSeneri: unyielding threads ran "):
        return re.sub(r"[0-9]+", "<work>", line)

    return line


def transcript_lines(path: pathlib.Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="strict").splitlines()


def expected_with_ledger(lines: list[str]) -> list[str]:
    if LEDGER_PROOF in lines:
        return lines

    try:
        completion = lines.index("ST PASS normal")
    except ValueError as error:
        raise ValueError("baseline has no ST PASS normal marker") from error

    return lines[:completion] + [LEDGER_PROOF] + lines[completion:]


def compare_transcripts(before: pathlib.Path, after: pathlib.Path) -> bool:
    expected = [normalize(line) for line in expected_with_ledger(
        transcript_lines(before)
    )]
    actual = [normalize(line) for line in transcript_lines(after)]

    if expected == actual:
        return True

    for line in difflib.unified_diff(
        expected,
        actual,
        fromfile=str(before),
        tofile=str(after),
        lineterm="",
    ):
        print(line)
    return False


def scenario_contract(repo: pathlib.Path) -> bool:
    test_source = (repo / "src/kernel/test.c").read_text(encoding="utf-8")
    makefile = (repo / "Makefile").read_text(encoding="utf-8")
    ok = True

    for scenario, (guest, host) in SCENARIOS.items():
        enum_name = "KERNEL_TEST_" + scenario.upper().replace("-", "_")
        guest_pattern = re.compile(
            rf"case {re.escape(enum_name)}:\s*return UINT8_C\({guest}\);"
        )
        host_pattern = re.compile(
            rf"^[ \t]*{re.escape(scenario)}\) expected={host} ;; \\\s*$",
            re.MULTILINE,
        )

        if guest_pattern.search(test_source) is None:
            print(f"scenario contract: {scenario} guest exit is not {guest}")
            ok = False
        if host_pattern.search(makefile) is None:
            print(f"scenario contract: {scenario} host status is not {host}")
            ok = False

    return ok


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=pathlib.Path)
    parser.add_argument("after", type=pathlib.Path)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path("."))
    arguments = parser.parse_args()

    transcript_ok = compare_transcripts(arguments.before, arguments.after)
    scenarios_ok = scenario_contract(arguments.repo)

    if not transcript_ok or not scenarios_ok:
        return 1

    print("boot transcript and 31-scenario exit contract match")
    return 0


if __name__ == "__main__":
    sys.exit(main())

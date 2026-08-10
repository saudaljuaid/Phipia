#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Inspect GCC stack-usage reports for bounded kernel paths."""

from __future__ import annotations

import argparse
from pathlib import Path


REQUIRED = {
    "interrupt_dispatch",
    "kernel_main",
    "scheduler_task_create",
    "scheduler_task_first_entry_c",
    "scheduler_task_reap",
    "scheduler_worker_task",
    "scheduler_yield",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--limit", type=int, required=True)
    arguments = parser.parse_args()
    reports = sorted(arguments.directory.rglob("*.su"))
    if not reports:
        parser.error("no GCC .su reports were produced")

    usage: dict[str, tuple[int, str]] = {}
    for report in reports:
        for raw_line in report.read_text(encoding="utf-8").splitlines():
            fields = raw_line.split("\t")
            if len(fields) < 3:
                parser.error(f"malformed stack-usage line: {raw_line!r}")
            location, byte_text, qualifier = fields[:3]
            function = location.rsplit(":", 1)[-1]
            try:
                byte_count = int(byte_text)
            except ValueError as error:
                parser.error(f"invalid stack byte count: {raw_line!r}")
                raise AssertionError from error
            if qualifier not in {"static", "dynamic,bounded"}:
                parser.error(f"unbounded stack usage: {raw_line!r}")
            previous = usage.get(function)
            if previous is None or byte_count > previous[0]:
                usage[function] = (byte_count, location)

    missing = sorted(REQUIRED - usage.keys())
    if missing:
        parser.error("required stack reports are missing: " + ", ".join(missing))
    largest = max(usage.items(), key=lambda item: item[1][0])
    if largest[1][0] > arguments.limit:
        parser.error(
            f"{largest[0]} uses {largest[1][0]} bytes, above {arguments.limit}"
        )

    print(
        f"stack-usage inspection passed {len(usage)} functions; "
        f"maximum {largest[0]}={largest[1][0]} bytes"
    )
    for function in sorted(REQUIRED):
        print(f"  {function}: {usage[function][0]} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

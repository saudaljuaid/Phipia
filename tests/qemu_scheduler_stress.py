#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run scheduler guests in competing TCG batches with exact protocol checks."""

from __future__ import annotations

import argparse
import concurrent.futures
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Scenario:
    name: str
    iso: Path
    status: int


def run_guest(qemu: str, ram: str, scenario: Scenario, instance: int) -> str:
    command = [
        qemu,
        "-machine",
        "accel=tcg",
        "-m",
        ram,
        "-smp",
        "1",
        "-cdrom",
        str(scenario.iso),
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "stdio",
        "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-no-reboot",
    ]
    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=15,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        raise RuntimeError(
            f"{scenario.name} instance {instance} timed out\n{output}"
        ) from error
    lines = completed.stdout.splitlines()
    begin = f"ZT BEGIN {scenario.name}"
    passed = f"ZT PASS {scenario.name}"
    valid = (
        completed.returncode == scenario.status
        and lines.count(begin) == 1
        and lines.count(passed) == 1
        and sum(line.startswith("ZT BEGIN ") for line in lines) == 1
        and sum(line.startswith("ZT PASS ") for line in lines) == 1
        and not any("ZT FAIL" in line or "Zenith OS PANIC" in line for line in lines)
    )
    if scenario.name == "scheduler-guard":
        valid = valid and any(
            "cr2=0xffffa00000000000" in line.lower() for line in lines
        ) and any(
            "page-fault bits: P=0 W=0 U=0 RSVD=0 I=0" in line
            for line in lines
        )
    if not valid:
        raise RuntimeError(
            f"{scenario.name} instance {instance} failed with "
            f"status {completed.returncode}\n{completed.stdout}"
        )
    return f"{scenario.name} instance {instance} passed"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--ram", required=True)
    parser.add_argument("--scheduler-iso", type=Path, required=True)
    parser.add_argument("--guard-iso", type=Path, required=True)
    parser.add_argument("--scheduler-runs", type=int, required=True)
    parser.add_argument("--guard-runs", type=int, required=True)
    parser.add_argument("--batch-size", type=int, required=True)
    arguments = parser.parse_args()
    if arguments.scheduler_runs < 20 or arguments.guard_runs < 1:
        parser.error("stress bounds are below the milestone requirement")
    if arguments.batch_size < 2:
        parser.error("batch size must create competing TCG load")
    if arguments.batch_size % 2 != 0:
        parser.error("batch size must permit balanced scenario contention")
    if arguments.guard_runs < arguments.scheduler_runs:
        parser.error("every scheduler guest requires a guard competitor")

    scheduler = Scenario("scheduler", arguments.scheduler_iso, 59)
    guard = Scenario("scheduler-guard", arguments.guard_iso, 61)
    jobs: list[tuple[Scenario, int]] = []
    scheduler_index = 0
    guard_index = 0
    while scheduler_index < arguments.scheduler_runs or guard_index < arguments.guard_runs:
        if scheduler_index < arguments.scheduler_runs:
            jobs.append((scheduler, scheduler_index))
            scheduler_index += 1
        if guard_index < arguments.guard_runs:
            jobs.append((guard, guard_index))
            guard_index += 1

    for start in range(0, len(jobs), arguments.batch_size):
        batch = jobs[start : start + arguments.batch_size]
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=len(batch)
        ) as executor:
            futures = [
                executor.submit(
                    run_guest,
                    arguments.qemu,
                    arguments.ram,
                    scenario,
                    instance,
                )
                for scenario, instance in batch
            ]
            for future in futures:
                print(future.result())

    print(
        f"scheduler stress passed {arguments.scheduler_runs} scheduler and "
        f"{arguments.guard_runs} guard guests in competing batches"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

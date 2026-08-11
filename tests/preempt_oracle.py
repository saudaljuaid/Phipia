#!/usr/bin/env python3
"""Independent randomized model for the bounded preemption state core."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass

CPU_LIMIT = 16
NESTING_LIMIT = 65_535

OK = 0
ALREADY_INITIALIZED = 2
NOT_INITIALIZED = 3
POISONED = 4
INVALID_CPU = 5
CPU_ALREADY_ONLINE = 6
CPU_OFFLINE = 7
LAST_CPU = 8
CPU_BUSY = 9
COUNTER_OVERFLOW = 10
COUNTER_UNDERFLOW = 11
NOT_ELIGIBLE = 12
CORRUPTED = 13


@dataclass
class Cpu:
    irq_depth: int = 0
    preempt_count: int = 0
    need_resched: bool = False
    online: bool = False


class Model:
    def __init__(self) -> None:
        self.cpus = [Cpu() for _ in range(CPU_LIMIT)]
        self.initialized = False
        self.poisoned = False

    @property
    def online_count(self) -> int:
        return sum(cpu.online for cpu in self.cpus)

    def initialize(self, boot_cpu: int) -> int:
        if self.initialized:
            return ALREADY_INITIALIZED
        if boot_cpu >= CPU_LIMIT:
            return INVALID_CPU
        self.__init__()
        self.cpus[boot_cpu].online = True
        self.initialized = True
        return OK

    def gate(self, cpu_index: int) -> tuple[int, Cpu | None]:
        if not self.initialized:
            return NOT_INITIALIZED, None
        if self.poisoned:
            return POISONED, None
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU, None
        cpu = self.cpus[cpu_index]
        if not cpu.online:
            return CPU_OFFLINE, None
        return OK, cpu

    @staticmethod
    def eligible(cpu: Cpu) -> bool:
        return (
            cpu.online
            and cpu.irq_depth == 0
            and cpu.preempt_count == 0
            and cpu.need_resched
        )

    def online(self, cpu_index: int) -> int:
        if not self.initialized:
            return NOT_INITIALIZED
        if self.poisoned:
            return POISONED
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU
        if self.cpus[cpu_index].online:
            return CPU_ALREADY_ONLINE
        self.cpus[cpu_index].online = True
        return OK

    def offline(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status
        assert cpu is not None
        if self.online_count == 1:
            return LAST_CPU
        if cpu.irq_depth or cpu.preempt_count or cpu.need_resched:
            return CPU_BUSY
        cpu.online = False
        return OK

    def irq_enter(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status
        assert cpu is not None
        if cpu.irq_depth == NESTING_LIMIT:
            self.poisoned = True
            return COUNTER_OVERFLOW
        cpu.irq_depth += 1
        return OK

    def irq_exit(self, cpu_index: int) -> tuple[int, bool]:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status, False
        assert cpu is not None
        if cpu.irq_depth == 0:
            self.poisoned = True
            return COUNTER_UNDERFLOW, False
        cpu.irq_depth -= 1
        return OK, self.eligible(cpu)

    def disable(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status
        assert cpu is not None
        if cpu.preempt_count == NESTING_LIMIT:
            self.poisoned = True
            return COUNTER_OVERFLOW
        cpu.preempt_count += 1
        return OK

    def enable(self, cpu_index: int) -> tuple[int, bool]:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status, False
        assert cpu is not None
        if cpu.preempt_count == 0:
            self.poisoned = True
            return COUNTER_UNDERFLOW, False
        cpu.preempt_count -= 1
        return OK, self.eligible(cpu)

    def request(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status == OK:
            assert cpu is not None
            cpu.need_resched = True
        return status

    def cancel(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status == OK:
            assert cpu is not None
            cpu.need_resched = False
        return status

    def query(self, cpu_index: int) -> tuple[int, bool]:
        status, cpu = self.gate(cpu_index)
        return status, self.eligible(cpu) if cpu is not None else False

    def take(self, cpu_index: int) -> int:
        status, cpu = self.gate(cpu_index)
        if status != OK:
            return status
        assert cpu is not None
        if not self.eligible(cpu):
            return NOT_ELIGIBLE
        cpu.need_resched = False
        return OK

    def snapshot(self) -> str:
        cpu_text = "".join(
            f" {cpu.irq_depth}:{cpu.preempt_count}:"
            f"{int(cpu.need_resched)}:{int(cpu.online)}"
            for cpu in self.cpus
        )
        return (
            f"S {OK} {int(self.initialized)} {int(self.poisoned)} "
            f"{self.online_count}{cpu_text}"
        )

    def cpu_snapshot(self, cpu_index: int) -> tuple[int, Cpu]:
        if not self.initialized:
            return NOT_INITIALIZED, Cpu()
        if self.poisoned:
            return POISONED, Cpu()
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU, Cpu()
        cpu = self.cpus[cpu_index]
        return OK, Cpu(
            irq_depth=cpu.irq_depth,
            preempt_count=cpu.preempt_count,
            need_resched=cpu.need_resched,
            online=cpu.online,
        )


def append(
    commands: list[str], expected: list[str], command: str, output: str
) -> None:
    commands.append(command)
    expected.append(output)


def exercise(
    commands: list[str], expected: list[str], cases: int, seed: int
) -> None:
    model = Model()
    rng = random.Random(seed)
    boot_cpu = rng.randrange(CPU_LIMIT)

    append(commands, expected, "Z", "Z")
    append(commands, expected, f"I {boot_cpu}", f"I {model.initialize(boot_cpu)}")
    append(
        commands,
        expected,
        f"I {boot_cpu}",
        f"I {model.initialize(boot_cpu)}",
    )

    for case in range(cases):
        cpu_index = rng.randrange(CPU_LIMIT + 4)
        choice = rng.randrange(100)
        if choice < 8:
            status = model.online(cpu_index)
            append(commands, expected, f"O {cpu_index}", f"O {status}")
        elif choice < 15:
            status = model.offline(cpu_index)
            append(commands, expected, f"F {cpu_index}", f"F {status}")
        elif choice < 29:
            status, cpu = model.gate(cpu_index)
            if status == OK and cpu is not None and cpu.irq_depth >= 32:
                status, value = model.irq_exit(cpu_index)
                append(commands, expected, f"X {cpu_index}", f"X {status} {int(value)}")
            else:
                status = model.irq_enter(cpu_index)
                append(commands, expected, f"N {cpu_index}", f"N {status}")
        elif choice < 43:
            status, cpu = model.gate(cpu_index)
            if status == OK and cpu is not None and cpu.irq_depth > 0:
                status, value = model.irq_exit(cpu_index)
                append(commands, expected, f"X {cpu_index}", f"X {status} {int(value)}")
            else:
                status = model.irq_enter(cpu_index)
                append(commands, expected, f"N {cpu_index}", f"N {status}")
        elif choice < 56:
            status, cpu = model.gate(cpu_index)
            if status == OK and cpu is not None and cpu.preempt_count >= 32:
                status, value = model.enable(cpu_index)
                append(commands, expected, f"E {cpu_index}", f"E {status} {int(value)}")
            else:
                status = model.disable(cpu_index)
                append(commands, expected, f"D {cpu_index}", f"D {status}")
        elif choice < 69:
            status, cpu = model.gate(cpu_index)
            if status == OK and cpu is not None and cpu.preempt_count > 0:
                status, value = model.enable(cpu_index)
                append(commands, expected, f"E {cpu_index}", f"E {status} {int(value)}")
            else:
                status = model.disable(cpu_index)
                append(commands, expected, f"D {cpu_index}", f"D {status}")
        elif choice < 80:
            status = model.request(cpu_index)
            append(commands, expected, f"R {cpu_index}", f"R {status}")
        elif choice < 87:
            status = model.cancel(cpu_index)
            append(commands, expected, f"C {cpu_index}", f"C {status}")
        elif choice < 92:
            status, value = model.query(cpu_index)
            append(commands, expected, f"G {cpu_index}", f"G {status} {int(value)}")
        elif choice < 96:
            status, cpu = model.cpu_snapshot(cpu_index)
            append(
                commands,
                expected,
                f"Q {cpu_index}",
                f"Q {status} {cpu.irq_depth} {cpu.preempt_count} "
                f"{int(cpu.need_resched)} {int(cpu.online)}",
            )
        else:
            status = model.take(cpu_index)
            append(commands, expected, f"T {cpu_index}", f"T {status}")

        if case % 4093 == 0:
            append(commands, expected, "S", model.snapshot())

    append(commands, expected, "S", model.snapshot())


def run_protocol(runner: str, commands: list[str], expected: list[str]) -> bool:
    completed = subprocess.run(
        [runner],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
        timeout=240,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        print(f"preempt-core runner exited {completed.returncode}", file=sys.stderr)
        return False
    observed = completed.stdout.splitlines()
    if len(observed) != len(expected):
        print(
            f"line-count mismatch: observed {len(observed)}, expected {len(expected)}",
            file=sys.stderr,
        )
        return False
    for line, (actual, wanted) in enumerate(zip(observed, expected, strict=True), 1):
        if actual != wanted:
            print(f"preemption oracle mismatch at line {line}", file=sys.stderr)
            print(f" command:  {commands[line - 1]}", file=sys.stderr)
            print(f" expected: {wanted}", file=sys.stderr)
            print(f" observed: {actual}", file=sys.stderr)
            return False
    return True


def boundary_cases(runner: str) -> bool:
    cases = [
        ("U\n", ["U 1"]),
        ("Q 0\n", [f"Q {NOT_INITIALIZED} 0 0 0 0"]),
        ("I 0\nQ 16\nQ 1\n", ["I 0", f"Q {INVALID_CPU} 0 0 0 0", "Q 0 0 0 0 0"]),
        ("I 0\nX 0\nV\n", ["I 0", f"X {COUNTER_UNDERFLOW} 0", f"V {POISONED}"]),
        ("I 0\nE 0\nV\n", ["I 0", f"E {COUNTER_UNDERFLOW} 0", f"V {POISONED}"]),
        ("I 0\nA 0\nN 0\nV\n", ["I 0", "A", f"N {COUNTER_OVERFLOW}", f"V {POISONED}"]),
        ("I 0\nB 0\nD 0\nV\n", ["I 0", "B", f"D {COUNTER_OVERFLOW}", f"V {POISONED}"]),
        (
            "I 0\nO 1\nR 0\nN 0\nN 0\nX 0\nX 0\nT 0\nS\n",
            [
                "I 0", "O 0", "R 0", "N 0", "N 0", "X 0 0",
                "X 0 1", "T 0",
                "S 0 1 0 2 0:0:0:1 0:0:0:1" + " 0:0:0:0" * 14,
            ],
        ),
        (
            "I 0\nO 1\nR 0\nD 0\nN 0\nX 0\nE 0\nT 0\n",
            ["I 0", "O 0", "R 0", "D 0", "N 0", "X 0 0", "E 0 1", "T 0"],
        ),
    ]
    for script, expected in cases:
        completed = subprocess.run(
            [runner], input=script, text=True, capture_output=True, check=False
        )
        if completed.returncode != 0 or completed.stdout.splitlines() != expected:
            print("preemption boundary case failed", file=sys.stderr)
            print(completed.stdout, file=sys.stderr, end="")
            return False

    for kind in range(13):
        completed = subprocess.run(
            [runner],
            input=f"I 0\nK {kind}\nV\n",
            text=True,
            capture_output=True,
            check=False,
        )
        expected = ["I 0", f"K {CORRUPTED} 1", f"V {POISONED}"]
        if completed.returncode != 0 or completed.stdout.splitlines() != expected:
            print(f"preemption corruption case {kind} failed", file=sys.stderr)
            print(completed.stdout, file=sys.stderr, end="")
            return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases", type=int, default=250_000)
    arguments = parser.parse_args()
    if arguments.cases < 4:
        parser.error("--cases must be at least four")

    commands: list[str] = []
    expected: list[str] = []
    seeds = [0x50524545, 0x4D505449, 0x4F4E434F, 0x52452121]
    base = arguments.cases // len(seeds)
    remainder = arguments.cases % len(seeds)
    for index, seed in enumerate(seeds):
        exercise(commands, expected, base + int(index < remainder), seed)

    if not run_protocol(arguments.runner, commands, expected):
        return 1
    if not boundary_cases(arguments.runner):
        return 1

    print(
        f"preemption oracle passed {arguments.cases} deterministic operations "
        f"across {len(seeds)} fixed seeds"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

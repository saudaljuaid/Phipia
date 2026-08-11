#!/usr/bin/env python3
"""Independent randomized model for bounded eager XSAVE ownership."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass

TASK_LIMIT = 16
CPU_LIMIT = 16
INVALID = 0xFFFFFFFF
UINT32_MAX = 0xFFFFFFFF
MASK = 0x7

OK = 0
ALREADY_INITIALIZED = 2
NOT_INITIALIZED = 3
POISONED = 4
INVALID_CPU = 5
CPU_ALREADY_ONLINE = 6
CPU_OFFLINE = 7
CPU_BUSY = 8
LAST_CPU = 9
NO_TASK_SLOT = 19
INVALID_HANDLE = 20
STALE_HANDLE = 21
GENERATION_EXHAUSTED = 22
INVALID_STATE = 23
IMAGE_NOT_READY = 24
IMAGE_ALREADY_READY = 25
CPU_ALREADY_LOADED = 26
TASK_ALREADY_LOADED = 27
TASK_NOT_LOADED = 28
OWNER_MISMATCH = 29
UNSUPPORTED_FEATURE = 14
UNEXPECTED_NM = 31

FREE = 0
BUILDING = 1
LIVE = 2
DYING = 3
RETIRED = 4


@dataclass
class Task:
    state: int = FREE
    generation: int = 0
    owner: int = INVALID
    saves: int = 0
    restores: int = 0
    ready: bool = False


@dataclass
class Cpu:
    online: bool = False
    task: int = INVALID
    generation: int = 0


class Model:
    def __init__(self) -> None:
        self.tasks = [Task() for _ in range(TASK_LIMIT)]
        self.cpus = [Cpu() for _ in range(CPU_LIMIT)]
        self.initialized = False
        self.poisoned = False
        self.nm_count = 0

    @property
    def active(self) -> int:
        return sum(task.state in (BUILDING, LIVE, DYING) for task in self.tasks)

    @property
    def retired(self) -> int:
        return sum(task.state == RETIRED for task in self.tasks)

    @property
    def online_count(self) -> int:
        return sum(cpu.online for cpu in self.cpus)

    def gate(self) -> int:
        if not self.initialized:
            return NOT_INITIALIZED
        if self.poisoned:
            return POISONED
        return OK

    def initialize(self, cpu: int) -> int:
        if self.initialized:
            return ALREADY_INITIALIZED
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        self.__init__()
        self.initialized = True
        self.cpus[cpu].online = True
        return OK

    def online_cpu(self, cpu: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        if self.cpus[cpu].online:
            return CPU_ALREADY_ONLINE
        self.cpus[cpu].online = True
        return OK

    def offline_cpu(self, cpu: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        item = self.cpus[cpu]
        if not item.online:
            return CPU_OFFLINE
        if self.online_count == 1:
            return LAST_CPU
        if item.task != INVALID:
            return CPU_BUSY
        self.cpus[cpu] = Cpu()
        return OK

    def resolve(self, index: int, generation: int) -> tuple[int, Task | None]:
        if index >= TASK_LIMIT or generation == 0:
            return INVALID_HANDLE, None
        task = self.tasks[index]
        if task.state in (FREE, RETIRED) or task.generation != generation:
            return STALE_HANDLE, None
        return OK, task

    def mutable(self, index: int, generation: int) -> tuple[int, Task | None]:
        status = self.gate()
        if status != OK:
            return status, None
        return self.resolve(index, generation)

    def create(self) -> tuple[int, int, int]:
        status = self.gate()
        if status != OK:
            return status, INVALID, 0
        for index, task in enumerate(self.tasks):
            if task.state == FREE:
                task.generation += 1
                task.state = BUILDING
                return OK, index, task.generation
        if self.retired == TASK_LIMIT:
            self.poisoned = True
            return GENERATION_EXHAUSTED, INVALID, 0
        return NO_TASK_SLOT, INVALID, 0

    def mark(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != BUILDING:
            return INVALID_STATE
        if task.ready:
            return IMAGE_ALREADY_READY
        task.ready = True
        return OK

    def publish(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != BUILDING:
            return INVALID_STATE
        if not task.ready:
            return IMAGE_NOT_READY
        task.state = LIVE
        return OK

    def release_slot(self, task: Task) -> None:
        generation = task.generation
        task.state = RETIRED if generation == UINT32_MAX else FREE
        task.owner = INVALID
        task.saves = 0
        task.restores = 0
        task.ready = False

    def abort(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != BUILDING:
            return INVALID_STATE
        self.release_slot(task)
        return OK

    def restore(self, cpu_index: int, index: int, generation: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU
        cpu = self.cpus[cpu_index]
        if not cpu.online:
            return CPU_OFFLINE
        status, task = self.resolve(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != LIVE:
            return INVALID_STATE
        if not task.ready:
            return IMAGE_NOT_READY
        if cpu.task != INVALID:
            return CPU_ALREADY_LOADED
        if task.owner != INVALID:
            return TASK_ALREADY_LOADED
        task.restores += 1
        task.owner = cpu_index
        cpu.task = index
        cpu.generation = generation
        return OK

    def save(self, cpu_index: int, index: int, generation: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU
        cpu = self.cpus[cpu_index]
        if not cpu.online:
            return CPU_OFFLINE
        status, task = self.resolve(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != LIVE:
            return INVALID_STATE
        if cpu.task == INVALID:
            return TASK_NOT_LOADED
        if cpu.task != index or cpu.generation != generation or task.owner != cpu_index:
            return OWNER_MISMATCH
        task.saves += 1
        task.owner = INVALID
        cpu.task = INVALID
        cpu.generation = 0
        return OK

    def begin_destroy(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != LIVE:
            return INVALID_STATE
        if task.owner != INVALID:
            return TASK_ALREADY_LOADED
        task.state = DYING
        return OK

    def abort_destroy(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != DYING:
            return INVALID_STATE
        task.state = LIVE
        return OK

    def reap(self, index: int, generation: int) -> int:
        status, task = self.mutable(index, generation)
        if status != OK:
            return status
        assert task is not None
        if task.state != DYING:
            return INVALID_STATE
        self.release_slot(task)
        return OK

    def require(self, mask: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if mask & ~MASK:
            self.poisoned = True
            return UNSUPPORTED_FEATURE
        return OK

    def nm(self, cpu_index: int) -> int:
        status = self.gate()
        if status != OK:
            return status
        if cpu_index >= CPU_LIMIT:
            return INVALID_CPU
        if not self.cpus[cpu_index].online:
            return CPU_OFFLINE
        self.nm_count += 1
        self.poisoned = True
        return UNEXPECTED_NM


def append(commands: list[str], expected: list[str], command: str, output: str) -> None:
    commands.append(command)
    expected.append(output)


def layout_boundaries(commands: list[str], expected: list[str]) -> None:
    cases = [
        ("L 0 1 1 0 7 7 832 832 256 576", "L 10 0 0 0 0 0 0"),
        ("L 1 0 1 0 7 7 832 832 256 576", "L 11 0 0 0 0 0 0"),
        ("L 1 1 0 0 7 7 832 832 256 576", "L 12 0 0 0 0 0 0"),
        ("L 1 1 1 1 7 7 832 832 256 576", "L 13 0 0 0 0 0 0"),
        ("L 1 1 1 0 6 7 832 832 256 576", "L 13 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 0 832 832 256 576", "L 13 0 0 0 0 0 0"),
        ("L 1 1 1 0 15 15 832 832 256 576", "L 14 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 5 832 832 256 576", "L 13 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 7 575 832 256 576", "L 15 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 7 833 832 256 576", "L 15 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 7 4097 4097 256 576", "L 15 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 7 4095 4096 256 576", "L 0 7 7 4096 64 256 576"),
        ("L 1 1 1 0 7 7 4096 4096 256 576", "L 0 7 7 4096 64 256 576"),
        ("L 1 1 1 0 7 7 832 832 255 576", "L 16 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 7 832 832 256 577", "L 16 0 0 0 0 0 0"),
        ("L 1 1 1 0 3 3 576 576 1 1", "L 16 0 0 0 0 0 0"),
        ("L 1 1 1 0 7 3 576 832 256 576", "L 0 3 7 576 64 0 0"),
        ("L 1 1 1 0 255 3 576 4096 256 576", "L 0 3 7 576 64 0 0"),
        ("L 1 1 1 0 7 7 833 900 256 576", "L 0 7 7 896 64 256 576"),
        ("L 1 1 1 0 7 7 832 832 256 576", "L 0 7 7 832 64 256 576"),
    ]
    for command, output in cases:
        append(commands, expected, command, output)
    append(commands, expected, "Y 64 832", "Y 0")
    append(commands, expected, "Y 65 832", "Y 17")
    append(commands, expected, "Y 0 832", "Y 17")
    append(commands, expected, "Y 64 831", "Y 18")
    append(commands, expected, "Y 18446744073709551552 832", "Y 15")


def choose_handle(rng: random.Random, known: list[tuple[int, int]]) -> tuple[int, int]:
    if known and rng.random() < 0.78:
        return rng.choice(known)
    return rng.randrange(0, TASK_LIMIT + 4), rng.choice([0, 1, UINT32_MAX])


def randomized_case(
    rng: random.Random,
    model: Model,
    known: list[tuple[int, int]],
) -> tuple[str, str]:
    if model.poisoned:
        model.__init__()
        return "W", "W 0"
    if not model.initialized:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        status = model.initialize(cpu)
        return f"I {cpu}", f"I {status}"

    choice = rng.randrange(100)
    if choice < 10:
        status, index, generation = model.create()
        if status == OK:
            known.append((index, generation))
        return "C", f"C {status} {index} {generation}"
    if choice < 18:
        index, generation = choose_handle(rng, known)
        status = model.mark(index, generation)
        return f"M {index} {generation}", f"M {status}"
    if choice < 26:
        index, generation = choose_handle(rng, known)
        status = model.publish(index, generation)
        return f"B {index} {generation}", f"B {status}"
    if choice < 31:
        index, generation = choose_handle(rng, known)
        status = model.abort(index, generation)
        return f"A {index} {generation}", f"A {status}"
    if choice < 42:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        index, generation = choose_handle(rng, known)
        status = model.restore(cpu, index, generation)
        return f"R {cpu} {index} {generation}", f"R {status}"
    if choice < 53:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        index, generation = choose_handle(rng, known)
        status = model.save(cpu, index, generation)
        return f"S {cpu} {index} {generation}", f"S {status}"
    if choice < 59:
        index, generation = choose_handle(rng, known)
        status = model.begin_destroy(index, generation)
        return f"D {index} {generation}", f"D {status}"
    if choice < 63:
        index, generation = choose_handle(rng, known)
        status = model.abort_destroy(index, generation)
        return f"U {index} {generation}", f"U {status}"
    if choice < 69:
        index, generation = choose_handle(rng, known)
        status = model.reap(index, generation)
        return f"X {index} {generation}", f"X {status}"
    if choice < 74:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        status = model.online_cpu(cpu)
        return f"O {cpu}", f"O {status}"
    if choice < 79:
        cpu = rng.randrange(0, CPU_LIMIT + 3)
        status = model.offline_cpu(cpu)
        return f"F {cpu}", f"F {status}"
    if choice < 84:
        index, generation = choose_handle(rng, known)
        status, task = model.resolve(index, generation)
        if status == OK:
            assert task is not None
            output = (
                f"Q 0 {task.state} {task.generation} {task.owner} "
                f"{task.saves} {task.restores} {int(task.ready)}"
            )
        else:
            output = f"Q {status} 0 0 {INVALID} 0 0 0"
        return f"Q {index} {generation}", output
    if choice < 89:
        cpu_index = rng.randrange(0, CPU_LIMIT + 3)
        if cpu_index >= CPU_LIMIT:
            status = INVALID_CPU
            cpu = Cpu()
        else:
            status = OK
            cpu = model.cpus[cpu_index]
        return (
            f"G {cpu_index}",
            f"G {status} {cpu.task} {cpu.generation} {int(cpu.online)}",
        )
    if choice < 93:
        return (
            "V",
            f"V 0 {model.active} {model.retired} {model.online_count} "
            f"{model.nm_count} {int(model.poisoned)}",
        )
    if choice < 98:
        mask = rng.choice([0, 1, 2, 3, 4, 7, 8, 0x100, 0xFFFFFFFFFFFFFFFF])
        status = model.require(mask)
        return f"E {mask}", f"E {status}"
    cpu = rng.randrange(0, CPU_LIMIT + 3)
    status = model.nm(cpu)
    return f"N {cpu}", f"N {status}"


def generation_boundary(commands: list[str], expected: list[str]) -> None:
    append(commands, expected, "W", "W 0")
    append(commands, expected, "I 0", "I 0")
    for index in range(TASK_LIMIT):
        append(commands, expected, f"J {index} {UINT32_MAX - 1}", "J 0")
        append(commands, expected, "C", f"C 0 {index} {UINT32_MAX}")
        append(commands, expected, f"A {index} {UINT32_MAX}", "A 0")
    append(commands, expected, "C", f"C {GENERATION_EXHAUSTED} {INVALID} 0")
    append(commands, expected, "V", f"V {POISONED} 0 {TASK_LIMIT} 1 0 1")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases", type=int, default=250_000)
    parser.add_argument("--seed", type=int, default=0x58534156)
    args = parser.parse_args()

    commands: list[str] = []
    expected: list[str] = []
    layout_boundaries(commands, expected)
    append(commands, expected, "W", "W 0")
    append(commands, expected, "I 0", "I 0")
    append(commands, expected, "H", "H 1")

    model = Model()
    model.initialize(0)
    known: list[tuple[int, int]] = []
    rng = random.Random(args.seed)
    while len(commands) < args.cases:
        command, output = randomized_case(rng, model, known)
        append(commands, expected, command, output)
    generation_boundary(commands, expected)

    result = subprocess.run(
        [args.runner],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        print(f"runner exited with {result.returncode}", file=sys.stderr)
        return 1
    actual = result.stdout.splitlines()
    if len(actual) != len(expected):
        print(f"line count mismatch: expected {len(expected)}, got {len(actual)}")
        return 1
    for index, (wanted, observed) in enumerate(zip(expected, actual, strict=True)):
        if wanted != observed:
            print(f"mismatch at operation {index}")
            print(f"command:  {commands[index]}")
            print(f"expected: {wanted}")
            print(f"actual:   {observed}")
            return 1

    print(
        f"xstate oracle passed: {len(expected)} operations, seed {args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

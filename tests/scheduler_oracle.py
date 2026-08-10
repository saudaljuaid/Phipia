#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the bounded C scheduler core with an independent Python model."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass


OK = 0
NULL_ARGUMENT = 1
ALREADY_INITIALIZED = 2
INVALID_HANDLE = 5
STALE_HANDLE = 6
GENERATION_EXHAUSTED = 7
DESCRIPTOR_LIMIT = 8
NO_RUNNABLE_PEER = 10
INVALID_STATE = 11
RUNNING_TASK = 12
RUNNABLE_TASK = 13
DOUBLE_REAP = 14
BOOTSTRAP_TASK = 15
CORRUPTED = 17

UNUSED = 0
CONSTRUCTING = 1
READY = 2
RUNNING = 3
EXITED = 4
REAPING = 5
RETIRED = 6

LIMIT = 16
INVALID_INDEX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


@dataclass(frozen=True)
class Identity:
    bootstrap: bool
    index: int = INVALID_INDEX
    generation: int = 0


@dataclass
class Descriptor:
    state: int = UNUSED
    generation: int = 1
    queued: bool = False


class Oracle:
    def __init__(self) -> None:
        self.descriptors = [Descriptor() for _ in range(LIMIT)]
        self.queue: list[Identity] = []
        self.queue_head = 0
        self.current = Identity(True)
        self.bootstrap_state = RUNNING
        self.successful_creations = 0
        self.failed_creations = 0
        self.context_switches = 0
        self.completed_tasks = 0
        self.reaped_tasks = 0

    def begin_create(self) -> tuple[int, int, int]:
        retired_seen = False
        for index, descriptor in enumerate(self.descriptors):
            if descriptor.state == RETIRED:
                retired_seen = True
                continue
            if descriptor.state != UNUSED:
                continue
            descriptor.state = CONSTRUCTING
            return OK, index, descriptor.generation
        return (
            GENERATION_EXHAUSTED if retired_seen else DESCRIPTOR_LIMIT,
            INVALID_INDEX,
            0,
        )

    def publish_create(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if state != CONSTRUCTING:
            return INVALID_STATE
        descriptor = self.descriptors[index]
        descriptor.state = READY
        descriptor.queued = True
        self.queue.append(Identity(False, index, generation))
        self.successful_creations += 1
        return OK

    def abort_create(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if state != CONSTRUCTING:
            return INVALID_STATE
        self.descriptors[index].state = UNUSED
        self.failed_creations += 1
        return OK

    def create(self) -> tuple[int, int, int]:
        status, index, generation = self.begin_create()
        if status == OK:
            assert self.publish_create(index, generation) == OK
        return status, index, generation

    def _make_ready(self, identity: Identity) -> None:
        if identity.bootstrap:
            self.bootstrap_state = READY
        else:
            descriptor = self.descriptors[identity.index]
            descriptor.state = READY
            descriptor.queued = True
        self.queue.append(identity)

    def _select(self) -> Identity:
        identity = self.queue.pop(0)
        self.queue_head = (self.queue_head + 1) % (LIMIT + 1)
        if identity.bootstrap:
            self.bootstrap_state = RUNNING
        else:
            descriptor = self.descriptors[identity.index]
            descriptor.state = RUNNING
            descriptor.queued = False
        self.current = identity
        return identity

    def yield_task(self) -> tuple[int, Identity | None, Identity | None]:
        if not self.queue:
            return NO_RUNNABLE_PEER, None, None
        previous = self.current
        self._make_ready(previous)
        next_identity = self._select()
        self.context_switches += 1
        return OK, previous, next_identity

    def exit_current(self) -> tuple[int, Identity | None, Identity | None]:
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, None, None
        if not self.queue:
            return NO_RUNNABLE_PEER, None, None
        previous = self.current
        self.descriptors[previous.index].state = EXITED
        next_identity = self._select()
        self.completed_tasks += 1
        self.context_switches += 1
        return OK, previous, next_identity

    def resolve(self, index: int, generation: int) -> tuple[int, int]:
        if index >= LIMIT or generation == 0:
            return INVALID_HANDLE, UNUSED
        descriptor = self.descriptors[index]
        if descriptor.generation != generation:
            return STALE_HANDLE, UNUSED
        return OK, descriptor.state

    def begin_reap(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        descriptor = self.descriptors[index]
        if state == RUNNING:
            return RUNNING_TASK
        if state == READY:
            return RUNNABLE_TASK
        if state in (UNUSED, RETIRED):
            return DOUBLE_REAP
        if state != EXITED:
            return INVALID_STATE
        self.descriptors[index].state = REAPING
        return OK

    def finish_reap(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if state != REAPING:
            return INVALID_STATE
        descriptor = self.descriptors[index]
        if descriptor.generation == UINT64_MAX:
            descriptor.state = RETIRED
        else:
            descriptor.generation += 1
            descriptor.state = UNUSED
        self.reaped_tasks += 1
        return OK

    def abort_reap(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if state != REAPING:
            return INVALID_STATE
        self.descriptors[index].state = EXITED
        return OK

    def reap(self, index: int, generation: int) -> int:
        status = self.begin_reap(index, generation)
        return self.finish_reap(index, generation) if status == OK else status

    @staticmethod
    def identity_text(identity: Identity) -> str:
        return (
            f" {int(identity.bootstrap)} {identity.index} "
            f"{identity.generation}"
        )

    def snapshot(self) -> str:
        fields = (
            f"S 0 {self.bootstrap_state} {self.queue_head} "
            f"{len(self.queue)}"
        )
        fields += self.identity_text(self.current)
        for identity in self.queue:
            fields += self.identity_text(identity)
        descriptors = ",".join(
            f"{item.state}:{item.generation}:{int(item.queued)}"
            for item in self.descriptors
        )
        return (
            f"{fields} | {descriptors} | {self.successful_creations} "
            f"{self.failed_creations} {self.context_switches} "
            f"{self.completed_tasks} {self.reaped_tasks}"
        )


def append(
    commands: list[str], expected: list[str], command: str, result: str
) -> None:
    commands.append(command)
    expected.append(result)


def exercise(
    commands: list[str], expected: list[str], cases: int, seed: int
) -> None:
    oracle = Oracle()
    rng = random.Random(seed)
    known_handles: list[tuple[int, int]] = []

    append(commands, expected, "I", "I 0")
    append(commands, expected, "J", f"J {ALREADY_INITIALIZED}")
    append(commands, expected, "N", "N 0")
    append(commands, expected, "Y", f"Y {NO_RUNNABLE_PEER}")

    for case in range(cases):
        choice = rng.randrange(100)
        if choice < 28:
            status, index, generation = oracle.create()
            append(
                commands,
                expected,
                "C",
                f"C {status} {index} {generation}",
            )
            if status == OK:
                known_handles.append((index, generation))
        elif choice < 56:
            status, previous, next_identity = oracle.yield_task()
            result = f"Y {status}"
            if status == OK:
                assert previous is not None and next_identity is not None
                result += oracle.identity_text(previous)
                result += oracle.identity_text(next_identity)
            append(commands, expected, "Y", result)
        elif choice < 68:
            status, previous, next_identity = oracle.exit_current()
            result = f"E {status}"
            if status == OK:
                assert previous is not None and next_identity is not None
                result += oracle.identity_text(previous)
                result += oracle.identity_text(next_identity)
            append(commands, expected, "E", result)
        elif choice < 86:
            if known_handles and rng.randrange(4) != 0:
                index, generation = rng.choice(known_handles)
            else:
                index = rng.randrange(LIMIT + 4)
                generation = rng.choice([0, 1, UINT64_MAX])
            status = oracle.reap(index, generation)
            append(commands, expected, f"R {index} {generation}", f"R {status}")
        else:
            if known_handles and rng.randrange(3) != 0:
                index, generation = rng.choice(known_handles)
            else:
                index = rng.randrange(LIMIT + 4)
                generation = rng.choice([0, 1, UINT64_MAX])
            status, state = oracle.resolve(index, generation)
            append(
                commands,
                expected,
                f"Q {index} {generation}",
                f"Q {status} {state if status == OK else 0}",
            )

        if case % 997 == 0:
            append(commands, expected, "S", oracle.snapshot())

    append(commands, expected, "S", oracle.snapshot())


def generation_boundary(commands: list[str], expected: list[str]) -> None:
    oracle = Oracle()
    append(commands, expected, "I", "I 0")
    for index in range(LIMIT):
        oracle.descriptors[index].generation = UINT64_MAX
        append(commands, expected, f"G {index} {UINT64_MAX}", "G 0")
        status, created_index, generation = oracle.create()
        append(
            commands,
            expected,
            "C",
            f"C {status} {created_index} {generation}",
        )
        status, previous, next_identity = oracle.yield_task()
        result = f"Y {status}"
        assert previous is not None and next_identity is not None
        result += oracle.identity_text(previous)
        result += oracle.identity_text(next_identity)
        append(commands, expected, "Y", result)
        status, previous, next_identity = oracle.exit_current()
        result = f"E {status}"
        assert previous is not None and next_identity is not None
        result += oracle.identity_text(previous)
        result += oracle.identity_text(next_identity)
        append(commands, expected, "E", result)
        status = oracle.reap(index, UINT64_MAX)
        append(commands, expected, f"R {index} {UINT64_MAX}", f"R {status}")
    status, index, generation = oracle.create()
    append(commands, expected, "C", f"C {status} {index} {generation}")


def transaction_boundaries(commands: list[str], expected: list[str]) -> None:
    oracle = Oracle()
    append(commands, expected, "I", "I 0")
    status, index, generation = oracle.begin_create()
    append(commands, expected, "B", f"B {status} {index} {generation}")
    status = oracle.abort_create(index, generation)
    append(commands, expected, f"A {index} {generation}", f"A {status}")
    status, task_state = oracle.resolve(index, generation)
    append(
        commands,
        expected,
        f"Q {index} {generation}",
        f"Q {status} {task_state}",
    )
    status, index, generation = oracle.begin_create()
    append(commands, expected, "B", f"B {status} {index} {generation}")
    status = oracle.publish_create(index, generation)
    append(commands, expected, f"P {index} {generation}", f"P {status}")
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status, previous, next_identity = oracle.yield_task()
    result = f"Y {status}"
    assert previous is not None and next_identity is not None
    result += oracle.identity_text(previous)
    result += oracle.identity_text(next_identity)
    append(commands, expected, "Y", result)
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status, previous, next_identity = oracle.exit_current()
    result = f"E {status}"
    assert previous is not None and next_identity is not None
    result += oracle.identity_text(previous)
    result += oracle.identity_text(next_identity)
    append(commands, expected, "E", result)
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status = oracle.abort_reap(index, generation)
    append(commands, expected, f"X {index} {generation}", f"X {status}")
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status = oracle.finish_reap(index, generation)
    append(commands, expected, f"F {index} {generation}", f"F {status}")
    status, _task_state = oracle.resolve(index, generation)
    append(
        commands,
        expected,
        f"Q {index} {generation}",
        f"Q {status} 0",
    )
    status = oracle.finish_reap(index, generation)
    append(commands, expected, f"F {index} {generation}", f"F {status}")
    append(commands, expected, "S", oracle.snapshot())


def corruption_cases(runner: str) -> bool:
    scripts = [
        "I\nC\nK 0\n",
        "I\nK 1\n",
        "I\nK 2\n",
        "I\nK 3\n",
        "I\nK 4\n",
        "I\nC\nK 5\n",
        "I\nK 6\n",
        "I\nC\nK 7\n",
        "I\nK 8\n",
        "I\nK 9\n",
    ]
    for script in scripts:
        completed = subprocess.run(
            [runner], input=script, text=True, capture_output=True, check=False
        )
        if completed.returncode != 0 or completed.stdout.splitlines()[-1] != "K 17":
            print("scheduler corruption case failed", file=sys.stderr)
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
    seeds = [0x53434845, 0x44554C45, 0x52524F42, 0x55535421]
    base = arguments.cases // len(seeds)
    remainder = arguments.cases % len(seeds)
    for index, seed in enumerate(seeds):
        exercise(commands, expected, base + int(index < remainder), seed)
    generation_boundary(commands, expected)
    transaction_boundaries(commands, expected)

    completed = subprocess.run(
        [arguments.runner],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
        timeout=240,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        print(f"scheduler-core runner exited {completed.returncode}", file=sys.stderr)
        observed_lines = completed.stdout.splitlines()
        print(
            f"runner produced {len(observed_lines)} of {len(commands)} lines",
            file=sys.stderr,
        )
        if len(observed_lines) < len(commands):
            start = max(0, len(observed_lines) - 2)
            for index in range(start, min(len(commands), len(observed_lines) + 3)):
                print(
                    f" protocol {index + 1}: {commands[index]}",
                    file=sys.stderr,
                )
        return 1
    observed = completed.stdout.splitlines()
    if len(observed) != len(expected):
        print(
            f"line-count mismatch: observed {len(observed)}, expected {len(expected)}",
            file=sys.stderr,
        )
        return 1
    for line, (actual, wanted) in enumerate(zip(observed, expected, strict=True), 1):
        if actual != wanted:
            print(f"scheduler oracle mismatch at protocol line {line}", file=sys.stderr)
            print(f" command:  {commands[line - 1]}", file=sys.stderr)
            print(f" expected: {wanted}", file=sys.stderr)
            print(f" observed: {actual}", file=sys.stderr)
            return 1
    if not corruption_cases(arguments.runner):
        return 1
    print(
        f"scheduler oracle passed {arguments.cases} deterministic operations "
        f"across {len(seeds)} fixed seeds"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

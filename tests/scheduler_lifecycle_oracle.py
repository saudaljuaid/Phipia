#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise scheduler lifecycle transitions against an independent model."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys

from scheduler_oracle import (
    BOOTSTRAP_TASK,
    DEFAULT_PRIORITY,
    INVALID_INDEX,
    LIMIT,
    OK,
    PRIORITY_MAX,
    QUEUE_CAPACITY,
    UINT64_MAX,
    Identity,
    Oracle,
)


FNV_OFFSET = 14_695_981_039_346_656_037
FNV_PRIME = 1_099_511_628_211


def hash_u64(current: int, value: int) -> int:
    for _ in range(8):
        current ^= value & 0xFF
        current = (current * FNV_PRIME) & UINT64_MAX
        value >>= 8
    return current


def state_hash(oracle: Oracle) -> str:
    current = FNV_OFFSET
    values = [
        oracle.monotonic_tick,
        int(oracle.current.bootstrap),
        oracle.current.index,
        oracle.current.generation,
        oracle.bootstrap_state,
        oracle.bootstrap_wait_age,
        oracle.queue_head,
        len(oracle.queue),
    ]
    for value in values:
        current = hash_u64(current, value)

    for offset in range(QUEUE_CAPACITY):
        identity = (
            oracle.queue[offset]
            if offset < len(oracle.queue)
            else Identity(False, INVALID_INDEX, 0)
        )
        current = hash_u64(current, int(identity.bootstrap))
        current = hash_u64(current, identity.index)
        current = hash_u64(current, identity.generation)

    for descriptor in oracle.descriptors:
        for value in (
            descriptor.state,
            descriptor.generation,
            int(descriptor.queued),
            descriptor.priority,
            descriptor.wait_age,
            descriptor.wait_event,
            descriptor.wait_epoch,
            descriptor.wake_deadline,
            descriptor.join_index,
            descriptor.join_generation,
            descriptor.join_completion_index,
            descriptor.join_completion_generation,
            int(descriptor.cancel_requested),
            int(descriptor.join_completion_ready),
        ):
            current = hash_u64(current, value)

    for index in range(len(oracle.event_epochs)):
        mask = sum(1 << waiter for waiter in oracle.event_waiters[index])
        current = hash_u64(current, oracle.event_epochs[index])
        current = hash_u64(current, mask)

    for waiters in oracle.join_waiters:
        current = hash_u64(current, sum(1 << waiter for waiter in waiters))

    for value in (
        oracle.successful_creations,
        oracle.failed_creations,
        oracle.context_switches,
        oracle.completed_tasks,
        oracle.reaped_tasks,
        oracle.blocked_transitions,
        oracle.wakeups,
        oracle.join_blocks,
        oracle.timed_sleeps,
        oracle.cancellation_requests,
        oracle.cancellations,
    ):
        current = hash_u64(current, value)

    return f"{current:016x}"


def add(
    commands: list[str],
    expected: list[str],
    command: str,
    prefix: str,
    _oracle: Oracle,
) -> None:
    commands.append(command)
    expected.append(prefix)


def snapshot(
    commands: list[str], expected: list[str], oracle: Oracle
) -> None:
    commands.append("S")
    expected.append(f"S {state_hash(oracle)}")


def choose_handle(
    rng: random.Random, known: list[tuple[int, int]]
) -> tuple[int, int]:
    if known and rng.randrange(5) != 0:
        return rng.choice(known)
    return rng.randrange(LIMIT + 4), rng.choice((0, 1, UINT64_MAX))


def random_campaign(
    commands: list[str],
    expected: list[str],
    cases: int,
    seed: int,
) -> None:
    oracle = Oracle()
    rng = random.Random(seed)
    known: list[tuple[int, int]] = []

    add(commands, expected, "I", "I 0", oracle)

    for case in range(cases):
        choice = rng.randrange(100)

        if choice < 11:
            priority = rng.randrange(PRIORITY_MAX + 3)
            status, index, generation = oracle.create(priority)
            if status == OK:
                known.append((index, generation))
            add(
                commands,
                expected,
                f"C {priority}",
                f"C {status} {index} {generation}",
                oracle,
            )

        elif choice < 24:
            status, _previous, _next = oracle.yield_task()
            add(commands, expected, "Y", f"Y {status}", oracle)
        elif choice < 31:
            status, _previous, _next = oracle.exit_current()
            add(commands, expected, "E", f"E {status}", oracle)
        elif choice < 45:
            index, generation = choose_handle(rng, known)
            status, completed, _previous, _next = oracle.join_current(
                index, generation
            )
            add(
                commands,
                expected,
                f"J {index} {generation}",
                f"J {status} {int(status == OK and completed)}",
                oracle,
            )
        elif choice < 52:
            status, index, generation = oracle.consume_join()
            add(
                commands,
                expected,
                "M",
                f"M {status} {index if status == OK else INVALID_INDEX} "
                f"{generation if status == OK else 0}",
                oracle,
            )
        elif choice < 63:
            index, generation = choose_handle(rng, known)
            status, made_runnable = oracle.request_cancel(index, generation)
            add(
                commands,
                expected,
                f"K {index} {generation}",
                f"K {status} {int(status == OK and made_runnable)}",
                oracle,
            )
        elif choice < 69:
            status, cancelled, _previous, _next = oracle.cancellation_point()
            add(
                commands,
                expected,
                "P",
                f"P {status} {int(status == OK and cancelled)}",
                oracle,
            )
        elif choice < 78:
            if rng.randrange(2) == 0:
                deadline = rng.choice(
                    (
                        oracle.monotonic_tick,
                        min(UINT64_MAX, oracle.monotonic_tick + rng.randrange(1, 33)),
                        UINT64_MAX,
                    )
                )
                status, _previous, _next = oracle.sleep_until(deadline)
                add(commands, expected, f"D {deadline}", f"D {status}", oracle)
            else:
                duration = rng.choice((0, rng.randrange(1, 33), UINT64_MAX))
                status, _previous, _next = oracle.sleep_for(duration)
                add(commands, expected, f"F {duration}", f"F {status}", oracle)
        elif choice < 89:
            if rng.randrange(4) == 0:
                status, woken = oracle.timer_tick()
                add(commands, expected, "T", f"T {status} {woken if status == OK else 0}", oracle)
            else:
                if rng.randrange(5) == 0 and oracle.monotonic_tick != 0:
                    tick = oracle.monotonic_tick - 1
                else:
                    tick = min(
                        UINT64_MAX,
                        oracle.monotonic_tick + rng.randrange(0, 65),
                    )
                status, woken = oracle.advance_time(tick)
                add(commands, expected, f"A {tick}", f"A {status} {woken if status == OK else 0}", oracle)
        elif choice < 96:
            index, generation = choose_handle(rng, known)
            status = oracle.reap(index, generation)
            add(commands, expected, f"R {index} {generation}", f"R {status}", oracle)
        else:
            index, generation = choose_handle(rng, known)
            priority = rng.randrange(PRIORITY_MAX + 3)
            status = oracle.set_priority(index, generation, priority)
            add(
                commands,
                expected,
                f"Z {index} {generation} {priority}",
                f"Z {status}",
                oracle,
            )

        if case % 997 == 0:
            snapshot(commands, expected, oracle)

    snapshot(commands, expected, oracle)


def directed_cases(commands: list[str], expected: list[str]) -> None:
    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    handles: list[tuple[int, int]] = []
    for _ in range(2):
        status, index, generation = oracle.create(DEFAULT_PRIORITY)
        handles.append((index, generation))
        add(commands, expected, "C 0", f"C {status} {index} {generation}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    target = handles[1]
    status, completed, _previous, _next = oracle.join_current(*target)
    add(commands, expected, f"J {target[0]} {target[1]}", f"J {status} {int(completed)}", oracle)
    status, _previous, _next = oracle.exit_current()
    add(commands, expected, "E", f"E {status}", oracle)
    status = oracle.reap(*target)
    add(commands, expected, f"R {target[0]} {target[1]}", f"R {status}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, index, generation = oracle.consume_join()
    add(commands, expected, "M", f"M {status} {index} {generation}", oracle)
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    pair: list[tuple[int, int]] = []
    for _ in range(2):
        status, index, generation = oracle.create()
        pair.append((index, generation))
        add(commands, expected, "C 0", f"C {status} {index} {generation}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, completed, _previous, _next = oracle.join_current(*pair[1])
    add(commands, expected, f"J {pair[1][0]} {pair[1][1]}", f"J {status} {int(completed)}", oracle)
    status, completed, _previous, _next = oracle.join_current(*pair[0])
    add(commands, expected, f"J {pair[0][0]} {pair[0][1]}", f"J {status} {int(completed)}", oracle)
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    status, index, generation = oracle.create()
    add(commands, expected, "C 0", f"C {status} {index} {generation}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, _previous, _next = oracle.sleep_for(10)
    add(commands, expected, "F 10", f"F {status}", oracle)
    status, made_runnable = oracle.request_cancel(index, generation)
    add(commands, expected, f"K {index} {generation}", f"K {status} {int(made_runnable)}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, cancelled, _previous, _next = oracle.cancellation_point()
    add(commands, expected, "P", f"P {status} {int(cancelled)}", oracle)
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    status, event_index, event_generation = oracle.create()
    add(
        commands,
        expected,
        "C 0",
        f"C {status} {event_index} {event_generation}",
        oracle,
    )
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    observe_status, epoch = oracle.observe_event(0)
    assert observe_status == OK
    status, _previous, _next = oracle.block_current(0, epoch)
    add(commands, expected, "B 0", f"B {status}", oracle)
    status, made_runnable = oracle.request_cancel(event_index, event_generation)
    add(
        commands,
        expected,
        f"K {event_index} {event_generation}",
        f"K {status} {int(made_runnable)}",
        oracle,
    )
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, cancelled, _previous, _next = oracle.cancellation_point()
    add(commands, expected, "P", f"P {status} {int(cancelled)}", oracle)
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    join_pair: list[tuple[int, int]] = []
    for _ in range(2):
        status, join_index, join_generation = oracle.create()
        join_pair.append((join_index, join_generation))
        add(
            commands,
            expected,
            "C 0",
            f"C {status} {join_index} {join_generation}",
            oracle,
        )
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, completed, _previous, _next = oracle.join_current(*join_pair[1])
    add(
        commands,
        expected,
        f"J {join_pair[1][0]} {join_pair[1][1]}",
        f"J {status} {int(completed)}",
        oracle,
    )
    status, made_runnable = oracle.request_cancel(*join_pair[0])
    add(
        commands,
        expected,
        f"K {join_pair[0][0]} {join_pair[0][1]}",
        f"K {status} {int(made_runnable)}",
        oracle,
    )
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    for _ in range(LIMIT):
        status, capacity_index, capacity_generation = oracle.create()
        add(
            commands,
            expected,
            "C 0",
            f"C {status} {capacity_index} {capacity_generation}",
            oracle,
        )
    status, capacity_index, capacity_generation = oracle.create()
    add(
        commands,
        expected,
        "C 0",
        f"C {status} {capacity_index} {capacity_generation}",
        oracle,
    )
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    status, low_index, low_generation = oracle.create(0)
    add(commands, expected, "C 0", f"C {status} {low_index} {low_generation}", oracle)
    status, high_index, high_generation = oracle.create(PRIORITY_MAX)
    add(
        commands,
        expected,
        f"C {PRIORITY_MAX}",
        f"C {status} {high_index} {high_generation}",
        oracle,
    )
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    status, _previous, _next = oracle.sleep_until(5)
    add(commands, expected, "D 5", f"D {status}", oracle)
    status, _previous, _next = oracle.sleep_until(5)
    add(commands, expected, "D 5", f"D {status}", oracle)
    status, woken = oracle.advance_time(5)
    add(commands, expected, "A 5", f"A {status} {woken}", oracle)
    status, _previous, _next = oracle.yield_task()
    add(commands, expected, "Y", f"Y {status}", oracle)
    snapshot(commands, expected, oracle)

    oracle = Oracle()
    add(commands, expected, "I", "I 0", oracle)
    oracle.monotonic_tick = UINT64_MAX
    add(commands, expected, f"G {UINT64_MAX}", "G 0", oracle)
    status, woken = oracle.timer_tick()
    add(commands, expected, "T", f"T {status} {woken}", oracle)
    snapshot(commands, expected, oracle)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases-per-seed", type=int, default=250_000)
    arguments = parser.parse_args()
    if arguments.cases_per_seed < 1:
        parser.error("--cases-per-seed must be positive")

    commands: list[str] = []
    expected: list[str] = []
    seeds = (0x4A4F494E, 0x43414E43, 0x534C4545, 0x54494D45)
    for seed in seeds:
        random_campaign(commands, expected, arguments.cases_per_seed, seed)
    directed_cases(commands, expected)

    completed = subprocess.run(
        [arguments.runner],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
        timeout=300,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        print(f"lifecycle runner exited {completed.returncode}", file=sys.stderr)
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
            print(f"lifecycle mismatch at line {line}", file=sys.stderr)
            print(f" command:  {commands[line - 1]}", file=sys.stderr)
            print(f" expected: {wanted}", file=sys.stderr)
            print(f" observed: {actual}", file=sys.stderr)
            return 1
    print(
        f"scheduler lifecycle oracle passed {arguments.cases_per_seed} operations "
        f"x {len(seeds)} fixed seeds"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

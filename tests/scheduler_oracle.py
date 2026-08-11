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
QUEUE_LIMIT = 9
NO_RUNNABLE_PEER = 10
INVALID_STATE = 11
RUNNING_TASK = 12
RUNNABLE_TASK = 13
DOUBLE_REAP = 14
BOOTSTRAP_TASK = 15
STATS_INVALID = 16
CORRUPTED = 17
INVALID_PRIORITY = 18
INVALID_EVENT = 19
EVENT_CHANGED = 20
EVENT_EPOCH_EXHAUSTED = 21
BLOCKED_TASK = 22
SELF_JOIN = 23
JOIN_CYCLE = 24
JOIN_COMPLETION_PENDING = 25
CANCELLATION_NOT_REQUESTED = 26
INVALID_DEADLINE = 27
TIME_REGRESSION = 28
TICK_EXHAUSTED = 29
DEADLINE_OVERFLOW = 30

UNUSED = 0
CONSTRUCTING = 1
READY = 2
RUNNING = 3
EXITED = 4
REAPING = 5
RETIRED = 6
BLOCKED = 8
JOIN_BLOCKED = 9
SLEEPING = 10

LIMIT = 16
QUEUE_CAPACITY = LIMIT + 1
EVENT_LIMIT = 8
INVALID_INDEX = (1 << 32) - 1
INVALID_EVENT_INDEX = (1 << 8) - 1
UINT64_MAX = (1 << 64) - 1
PRIORITY_MIN = 0
PRIORITY_MAX = 7
DEFAULT_PRIORITY = PRIORITY_MIN
AGING_INTERVAL = 8
MAXIMUM_WAIT_AGE = PRIORITY_MAX * AGING_INTERVAL


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
    priority: int = DEFAULT_PRIORITY
    wait_age: int = 0
    wait_event: int = INVALID_EVENT_INDEX
    wait_epoch: int = 0
    wake_deadline: int = 0
    join_index: int = INVALID_INDEX
    join_generation: int = 0
    join_completion_index: int = INVALID_INDEX
    join_completion_generation: int = 0
    cancel_requested: bool = False
    join_completion_ready: bool = False


class Oracle:
    def __init__(self) -> None:
        self.descriptors = [Descriptor() for _ in range(LIMIT)]
        self.queue: list[Identity] = []
        self.queue_head = 0
        self.current = Identity(True)
        self.bootstrap_state = RUNNING
        self.bootstrap_wait_age = 0
        self.event_epochs = [1 for _ in range(EVENT_LIMIT)]
        self.event_waiters = [set() for _ in range(EVENT_LIMIT)]
        self.join_waiters = [set() for _ in range(LIMIT)]
        self.monotonic_tick = 0
        self.successful_creations = 0
        self.failed_creations = 0
        self.context_switches = 0
        self.completed_tasks = 0
        self.reaped_tasks = 0
        self.blocked_transitions = 0
        self.wakeups = 0
        self.join_blocks = 0
        self.timed_sleeps = 0
        self.cancellation_requests = 0
        self.cancellations = 0

    @staticmethod
    def priority_valid(priority: int) -> bool:
        return PRIORITY_MIN <= priority <= PRIORITY_MAX

    @staticmethod
    def _clear_runtime(descriptor: Descriptor) -> None:
        descriptor.priority = DEFAULT_PRIORITY
        descriptor.wait_age = 0
        descriptor.queued = False
        descriptor.wait_event = INVALID_EVENT_INDEX
        descriptor.wait_epoch = 0
        descriptor.wake_deadline = 0
        descriptor.join_index = INVALID_INDEX
        descriptor.join_generation = 0
        descriptor.join_completion_index = INVALID_INDEX
        descriptor.join_completion_generation = 0
        descriptor.cancel_requested = False
        descriptor.join_completion_ready = False

    @staticmethod
    def _clear_wait(descriptor: Descriptor) -> None:
        descriptor.wait_event = INVALID_EVENT_INDEX
        descriptor.wait_epoch = 0
        descriptor.wake_deadline = 0
        descriptor.join_index = INVALID_INDEX
        descriptor.join_generation = 0

    @staticmethod
    def _clear_completion(descriptor: Descriptor) -> None:
        descriptor.join_completion_index = INVALID_INDEX
        descriptor.join_completion_generation = 0
        descriptor.join_completion_ready = False

    def begin_create(self, priority: int = DEFAULT_PRIORITY) -> tuple[int, int, int]:
        if not self.priority_valid(priority):
            return INVALID_PRIORITY, INVALID_INDEX, 0
        retired_seen = False
        for index, descriptor in enumerate(self.descriptors):
            if descriptor.state == RETIRED:
                retired_seen = True
                continue
            if descriptor.state != UNUSED:
                continue
            descriptor.state = CONSTRUCTING
            descriptor.priority = priority
            descriptor.wait_age = 0
            descriptor.queued = False
            descriptor.wait_event = INVALID_EVENT_INDEX
            descriptor.wait_epoch = 0
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
        self._make_ready(Identity(False, index, generation))
        self.successful_creations += 1
        return OK

    def abort_create(self, index: int, generation: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if state != CONSTRUCTING:
            return INVALID_STATE
        descriptor = self.descriptors[index]
        descriptor.state = UNUSED
        self._clear_runtime(descriptor)
        self.failed_creations += 1
        return OK

    def create(self, priority: int = DEFAULT_PRIORITY) -> tuple[int, int, int]:
        status, index, generation = self.begin_create(priority)
        if status == OK:
            assert self.publish_create(index, generation) == OK
        return status, index, generation

    def set_priority(self, index: int, generation: int, priority: int) -> int:
        status, state = self.resolve(index, generation)
        if status != OK:
            return status
        if not self.priority_valid(priority):
            return INVALID_PRIORITY
        if state not in (
            CONSTRUCTING, READY, RUNNING, BLOCKED, JOIN_BLOCKED, SLEEPING
        ):
            return INVALID_STATE
        descriptor = self.descriptors[index]
        descriptor.priority = priority
        descriptor.wait_age = 0
        return OK

    def _make_ready(self, identity: Identity) -> None:
        if identity.bootstrap:
            self.bootstrap_state = READY
            self.bootstrap_wait_age = 0
        else:
            descriptor = self.descriptors[identity.index]
            descriptor.state = READY
            descriptor.queued = True
            descriptor.wait_age = 0
            descriptor.wait_event = INVALID_EVENT_INDEX
            descriptor.wait_epoch = 0
            descriptor.wake_deadline = 0
            descriptor.join_index = INVALID_INDEX
            descriptor.join_generation = 0
        self.queue.append(identity)

    def _effective_priority(self, identity: Identity) -> int:
        if identity.bootstrap:
            priority = DEFAULT_PRIORITY
            age = self.bootstrap_wait_age
        else:
            descriptor = self.descriptors[identity.index]
            priority = descriptor.priority
            age = descriptor.wait_age
        return min(PRIORITY_MAX, priority + age // AGING_INTERVAL)

    def _age_ready(self) -> None:
        for identity in self.queue:
            if identity.bootstrap:
                self.bootstrap_wait_age = min(
                    MAXIMUM_WAIT_AGE, self.bootstrap_wait_age + 1
                )
            else:
                descriptor = self.descriptors[identity.index]
                descriptor.wait_age = min(
                    MAXIMUM_WAIT_AGE, descriptor.wait_age + 1
                )

    def _select(self) -> Identity:
        effective = [self._effective_priority(item) for item in self.queue]
        selected_offset = effective.index(max(effective))
        identity = self.queue.pop(selected_offset)
        if selected_offset == 0:
            self.queue_head = (self.queue_head + 1) % (LIMIT + 1)
        if identity.bootstrap:
            self.bootstrap_state = RUNNING
            self.bootstrap_wait_age = 0
        else:
            descriptor = self.descriptors[identity.index]
            descriptor.state = RUNNING
            descriptor.queued = False
            descriptor.wait_age = 0
        self.current = identity
        self._age_ready()
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
        descriptor = self.descriptors[previous.index]
        descriptor.state = EXITED
        descriptor.queued = False
        descriptor.wait_age = 0
        descriptor.cancel_requested = False
        self._clear_wait(descriptor)
        self._clear_completion(descriptor)
        waiters = sorted(self.join_waiters[previous.index])
        for index in waiters:
            waiter = self.descriptors[index]
            self._clear_wait(waiter)
            waiter.join_completion_index = previous.index
            waiter.join_completion_generation = previous.generation
            waiter.join_completion_ready = True
            self._make_ready(Identity(False, index, waiter.generation))
        self.join_waiters[previous.index].clear()
        next_identity = self._select()
        self.completed_tasks += 1
        self.context_switches += 1
        self.wakeups += len(waiters)
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
        if state in (BLOCKED, JOIN_BLOCKED, SLEEPING):
            return BLOCKED_TASK
        if state in (UNUSED, RETIRED):
            return DOUBLE_REAP
        if state != EXITED:
            return INVALID_STATE
        descriptor.state = REAPING
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
        self._clear_runtime(descriptor)
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

    def observe_event(self, event_index: int) -> tuple[int, int]:
        if event_index >= EVENT_LIMIT:
            return INVALID_EVENT, 0
        return OK, self.event_epochs[event_index]

    def block_current(
        self, event_index: int, observed_epoch: int
    ) -> tuple[int, Identity | None, Identity | None]:
        if event_index >= EVENT_LIMIT:
            return INVALID_EVENT, None, None
        if self.event_epochs[event_index] == UINT64_MAX:
            return EVENT_EPOCH_EXHAUSTED, None, None
        if self.event_epochs[event_index] != observed_epoch:
            return EVENT_CHANGED, None, None
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, None, None
        if self.descriptors[self.current.index].join_completion_ready:
            return JOIN_COMPLETION_PENDING, None, None
        if not self.queue:
            return NO_RUNNABLE_PEER, None, None
        previous = self.current
        descriptor = self.descriptors[previous.index]
        descriptor.state = BLOCKED
        descriptor.queued = False
        descriptor.wait_age = 0
        descriptor.wait_event = event_index
        descriptor.wait_epoch = observed_epoch
        self.event_waiters[event_index].add(previous.index)
        next_identity = self._select()
        self.blocked_transitions += 1
        self.context_switches += 1
        return OK, previous, next_identity

    def signal_event(self, event_index: int, wake_all: bool) -> tuple[int, int, int]:
        if event_index >= EVENT_LIMIT:
            return INVALID_EVENT, 0, 0
        if self.event_epochs[event_index] == UINT64_MAX:
            return EVENT_EPOCH_EXHAUSTED, 0, 0
        waiters = sorted(self.event_waiters[event_index])
        selected = waiters if wake_all else waiters[:1]
        if self.wakeups > UINT64_MAX - len(selected):
            return STATS_INVALID, 0, 0
        if len(self.queue) > QUEUE_CAPACITY - len(selected):
            return QUEUE_LIMIT, 0, 0
        self.event_epochs[event_index] += 1
        selected_set = set(selected)
        for index in waiters:
            descriptor = self.descriptors[index]
            if index in selected_set:
                self.event_waiters[event_index].remove(index)
                self._make_ready(Identity(False, index, descriptor.generation))
            else:
                descriptor.wait_epoch = self.event_epochs[event_index]
        self.wakeups += len(selected)
        return OK, len(selected), self.event_epochs[event_index]

    @staticmethod
    def _incomplete(state: int) -> bool:
        return state in (READY, RUNNING, BLOCKED, JOIN_BLOCKED, SLEEPING)

    def _join_would_cycle(self, waiter_index: int, target_index: int) -> bool:
        current = target_index
        for _ in range(LIMIT):
            if current == waiter_index:
                return True
            descriptor = self.descriptors[current]
            if descriptor.state != JOIN_BLOCKED:
                return False
            current = descriptor.join_index
        return True

    def join_current(
        self, index: int, generation: int
    ) -> tuple[int, bool, Identity | None, Identity | None]:
        status, target_state = self.resolve(index, generation)
        if status != OK:
            return status, False, None, None
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, False, None, None
        waiter = self.descriptors[self.current.index]
        if waiter.join_completion_ready:
            return JOIN_COMPLETION_PENDING, False, None, None
        if index == self.current.index and generation == self.current.generation:
            return SELF_JOIN, False, None, None
        if target_state in (EXITED, REAPING):
            waiter.join_completion_index = index
            waiter.join_completion_generation = generation
            waiter.join_completion_ready = True
            return OK, True, None, None
        if not self._incomplete(target_state):
            if target_state in (UNUSED, RETIRED):
                return DOUBLE_REAP, False, None, None
            return INVALID_STATE, False, None, None
        if self._join_would_cycle(self.current.index, index):
            return JOIN_CYCLE, False, None, None
        if not self.queue:
            return NO_RUNNABLE_PEER, False, None, None
        previous = self.current
        waiter.state = JOIN_BLOCKED
        waiter.queued = False
        waiter.wait_age = 0
        self._clear_wait(waiter)
        waiter.join_index = index
        waiter.join_generation = generation
        self.join_waiters[index].add(previous.index)
        next_identity = self._select()
        self.blocked_transitions += 1
        self.join_blocks += 1
        self.context_switches += 1
        return OK, False, previous, next_identity

    def consume_join(self) -> tuple[int, int, int]:
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, INVALID_INDEX, 0
        descriptor = self.descriptors[self.current.index]
        if not descriptor.join_completion_ready:
            return INVALID_STATE, INVALID_INDEX, 0
        index = descriptor.join_completion_index
        generation = descriptor.join_completion_generation
        self._clear_completion(descriptor)
        return OK, index, generation

    def request_cancel(self, index: int, generation: int) -> tuple[int, bool]:
        status, task_state = self.resolve(index, generation)
        if status != OK:
            return status, False
        descriptor = self.descriptors[index]
        if not self._incomplete(task_state):
            return INVALID_STATE, False
        if descriptor.cancel_requested:
            return OK, False
        blocked = task_state in (BLOCKED, JOIN_BLOCKED, SLEEPING)
        descriptor.cancel_requested = True
        if task_state == BLOCKED:
            self.event_waiters[descriptor.wait_event].remove(index)
        elif task_state == JOIN_BLOCKED:
            self.join_waiters[descriptor.join_index].remove(index)
        if blocked:
            self._clear_wait(descriptor)
            self._make_ready(Identity(False, index, generation))
            self.wakeups += 1
        self.cancellation_requests += 1
        return OK, blocked

    def cancellation_point(
        self,
    ) -> tuple[int, bool, Identity | None, Identity | None]:
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, False, None, None
        if not self.descriptors[self.current.index].cancel_requested:
            return CANCELLATION_NOT_REQUESTED, False, None, None
        if not self.queue:
            return NO_RUNNABLE_PEER, False, None, None
        status, previous, next_identity = self.exit_current()
        if status != OK:
            return status, False, None, None
        self.cancellations += 1
        return OK, True, previous, next_identity

    def sleep_until(
        self, deadline: int
    ) -> tuple[int, Identity | None, Identity | None]:
        if self.current.bootstrap:
            return BOOTSTRAP_TASK, None, None
        descriptor = self.descriptors[self.current.index]
        if descriptor.join_completion_ready:
            return JOIN_COMPLETION_PENDING, None, None
        if deadline <= self.monotonic_tick:
            return INVALID_DEADLINE, None, None
        if not self.queue:
            return NO_RUNNABLE_PEER, None, None
        previous = self.current
        descriptor.state = SLEEPING
        descriptor.queued = False
        descriptor.wait_age = 0
        self._clear_wait(descriptor)
        descriptor.wake_deadline = deadline
        next_identity = self._select()
        self.blocked_transitions += 1
        self.timed_sleeps += 1
        self.context_switches += 1
        return OK, previous, next_identity

    def sleep_for(
        self, duration: int
    ) -> tuple[int, Identity | None, Identity | None]:
        if duration == 0:
            return INVALID_DEADLINE, None, None
        if duration > UINT64_MAX - self.monotonic_tick:
            return DEADLINE_OVERFLOW, None, None
        return self.sleep_until(self.monotonic_tick + duration)

    def advance_time(self, new_tick: int) -> tuple[int, int]:
        if new_tick < self.monotonic_tick:
            return TIME_REGRESSION, 0
        if new_tick == self.monotonic_tick:
            return OK, 0
        waking = [
            index
            for index, descriptor in enumerate(self.descriptors)
            if descriptor.state == SLEEPING and descriptor.wake_deadline <= new_tick
        ]
        self.monotonic_tick = new_tick
        for index in waking:
            descriptor = self.descriptors[index]
            self._clear_wait(descriptor)
            self._make_ready(Identity(False, index, descriptor.generation))
        self.wakeups += len(waking)
        return OK, len(waking)

    def timer_tick(self) -> tuple[int, int]:
        if self.monotonic_tick == UINT64_MAX:
            return TICK_EXHAUSTED, 0
        return self.advance_time(self.monotonic_tick + 1)

    @staticmethod
    def identity_text(identity: Identity) -> str:
        return f" {int(identity.bootstrap)} {identity.index} {identity.generation}"

    def snapshot(self) -> str:
        fields = (
            f"S 0 {self.bootstrap_state} {self.queue_head} "
            f"{len(self.queue)}"
        )
        fields += self.identity_text(self.current)
        for identity in self.queue:
            fields += self.identity_text(identity)
        descriptors = ",".join(
            f"{item.state}:{item.generation}:{int(item.queued)}:"
            f"{item.priority}:{item.wait_age}:{item.wait_event}:"
            f"{item.wait_epoch}"
            for item in self.descriptors
        )
        events = ",".join(
            f"{self.event_epochs[index]}:"
            f"{sum(1 << waiter for waiter in self.event_waiters[index])}"
            for index in range(EVENT_LIMIT)
        )
        return (
            f"{fields} | {descriptors} | {events} | "
            f"{self.successful_creations} {self.failed_creations} "
            f"{self.context_switches} {self.completed_tasks} "
            f"{self.reaped_tasks} {self.blocked_transitions} {self.wakeups}"
        )


def append(
    commands: list[str], expected: list[str], command: str, result: str
) -> None:
    commands.append(command)
    expected.append(result)


def append_switch(
    commands: list[str],
    expected: list[str],
    command: str,
    operation: str,
    result: tuple[int, Identity | None, Identity | None],
) -> None:
    status, previous, next_identity = result
    output = f"{operation} {status}"
    if status == OK:
        assert previous is not None and next_identity is not None
        output += Oracle.identity_text(previous)
        output += Oracle.identity_text(next_identity)
    append(commands, expected, command, output)


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
        if choice < 18:
            priority = rng.randrange(PRIORITY_MAX + 3)
            status, index, generation = oracle.create(priority)
            append(
                commands,
                expected,
                f"H {priority}",
                f"H {status} {index} {generation}",
            )
            if status == OK:
                known_handles.append((index, generation))
        elif choice < 40:
            append_switch(commands, expected, "Y", "Y", oracle.yield_task())
        elif choice < 49:
            append_switch(commands, expected, "E", "E", oracle.exit_current())
        elif choice < 61:
            if known_handles and rng.randrange(4) != 0:
                index, generation = rng.choice(known_handles)
            else:
                index = rng.randrange(LIMIT + 4)
                generation = rng.choice([0, 1, UINT64_MAX])
            status = oracle.reap(index, generation)
            append(commands, expected, f"R {index} {generation}", f"R {status}")
        elif choice < 70:
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
        elif choice < 79:
            if known_handles and rng.randrange(4) != 0:
                index, generation = rng.choice(known_handles)
            else:
                index = rng.randrange(LIMIT + 4)
                generation = rng.choice([0, 1, UINT64_MAX])
            priority = rng.randrange(PRIORITY_MAX + 3)
            status = oracle.set_priority(index, generation, priority)
            append(
                commands,
                expected,
                f"Z {index} {generation} {priority}",
                f"Z {status}",
            )
        elif choice < 90:
            event_index = rng.randrange(EVENT_LIMIT + 3)
            status, epoch = oracle.observe_event(event_index)
            append(
                commands,
                expected,
                f"O {event_index}",
                f"O {status} {epoch if status == OK else 0}",
            )
            observed = epoch
            if status == OK and rng.randrange(4) == 0:
                observed = epoch - 1 if epoch > 1 else epoch + 1
            result = oracle.block_current(event_index, observed)
            append_switch(
                commands,
                expected,
                f"W {event_index} {observed}",
                "W",
                result,
            )
        else:
            event_index = rng.randrange(EVENT_LIMIT + 3)
            wake_all = rng.randrange(4) == 0
            status, count, epoch = oracle.signal_event(event_index, wake_all)
            append(
                commands,
                expected,
                f"L {event_index} {int(wake_all)}",
                f"L {status} {count if status == OK else 0} "
                f"{epoch if status == OK else 0}",
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
        append(commands, expected, "C", f"C {status} {created_index} {generation}")
        append_switch(commands, expected, "Y", "Y", oracle.yield_task())
        append_switch(commands, expected, "E", "E", oracle.exit_current())
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
    append(commands, expected, f"Q {index} {generation}", f"Q {status} {task_state}")
    status, index, generation = oracle.begin_create()
    append(commands, expected, "B", f"B {status} {index} {generation}")
    status = oracle.publish_create(index, generation)
    append(commands, expected, f"P {index} {generation}", f"P {status}")
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    append_switch(commands, expected, "Y", "Y", oracle.yield_task())
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    append_switch(commands, expected, "E", "E", oracle.exit_current())
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status = oracle.abort_reap(index, generation)
    append(commands, expected, f"X {index} {generation}", f"X {status}")
    status = oracle.begin_reap(index, generation)
    append(commands, expected, f"D {index} {generation}", f"D {status}")
    status = oracle.finish_reap(index, generation)
    append(commands, expected, f"F {index} {generation}", f"F {status}")
    status, _task_state = oracle.resolve(index, generation)
    append(commands, expected, f"Q {index} {generation}", f"Q {status} 0")
    status = oracle.finish_reap(index, generation)
    append(commands, expected, f"F {index} {generation}", f"F {status}")
    append(commands, expected, "S", oracle.snapshot())


def priority_event_boundaries(commands: list[str], expected: list[str]) -> None:
    oracle = Oracle()
    append(commands, expected, "I", "I 0")
    low_status, low_index, low_generation = oracle.create(PRIORITY_MIN)
    high_status, high_index, high_generation = oracle.create(PRIORITY_MAX)
    append(
        commands,
        expected,
        f"H {PRIORITY_MIN}",
        f"H {low_status} {low_index} {low_generation}",
    )
    append(
        commands,
        expected,
        f"H {PRIORITY_MAX}",
        f"H {high_status} {high_index} {high_generation}",
    )

    append_switch(commands, expected, "Y", "Y", oracle.yield_task())
    assert oracle.current.index == high_index

    serviced_low = False
    for _ in range(MAXIMUM_WAIT_AGE + LIMIT + 1):
        append_switch(commands, expected, "Y", "Y", oracle.yield_task())
        if not oracle.current.bootstrap and oracle.current.index == low_index:
            serviced_low = True
            break
    assert serviced_low, "aging failed to provide bounded low-priority service"

    status = oracle.set_priority(low_index, low_generation, PRIORITY_MAX + 1)
    append(
        commands,
        expected,
        f"Z {low_index} {low_generation} {PRIORITY_MAX + 1}",
        f"Z {status}",
    )

    status, epoch = oracle.observe_event(0)
    append(commands, expected, "O 0", f"O {status} {epoch}")
    append_switch(
        commands,
        expected,
        f"W 0 {epoch}",
        "W",
        oracle.block_current(0, epoch),
    )
    status, count, new_epoch = oracle.signal_event(0, False)
    append(commands, expected, "L 0 0", f"L {status} {count} {new_epoch}")
    append_switch(
        commands,
        expected,
        f"W 0 {epoch}",
        "W",
        oracle.block_current(0, epoch),
    )
    append(commands, expected, "S", oracle.snapshot())


def event_boundaries(commands: list[str], expected: list[str]) -> None:
    oracle = Oracle()
    handles: list[tuple[int, int]] = []
    append(commands, expected, "I", "I 0")

    for _ in range(3):
        status, index, generation = oracle.create()
        handles.append((index, generation))
        append(commands, expected, "C", f"C {status} {index} {generation}")

    append_switch(commands, expected, "Y", "Y", oracle.yield_task())
    status, epoch = oracle.observe_event(2)
    append(commands, expected, "O 2", f"O {status} {epoch}")

    for _ in range(3):
        append_switch(
            commands,
            expected,
            f"W 2 {epoch}",
            "W",
            oracle.block_current(2, epoch),
        )

    blocked_index, blocked_generation = handles[0]
    status = oracle.begin_reap(blocked_index, blocked_generation)
    append(
        commands,
        expected,
        f"D {blocked_index} {blocked_generation}",
        f"D {status}",
    )
    status, count, new_epoch = oracle.signal_event(2, False)
    append(commands, expected, "L 2 0", f"L {status} {count} {new_epoch}")
    append_switch(
        commands,
        expected,
        f"W 2 {epoch}",
        "W",
        oracle.block_current(2, epoch),
    )
    status, count, final_epoch = oracle.signal_event(2, True)
    append(commands, expected, "L 2 1", f"L {status} {count} {final_epoch}")

    oracle.event_epochs[3] = UINT64_MAX
    append(commands, expected, f"U 3 {UINT64_MAX}", "U 0")
    status, exhausted_epoch = oracle.observe_event(3)
    append(commands, expected, "O 3", f"O {status} {exhausted_epoch}")
    append_switch(
        commands,
        expected,
        f"W 3 {UINT64_MAX}",
        "W",
        oracle.block_current(3, UINT64_MAX),
    )
    status, count, signal_epoch = oracle.signal_event(3, True)
    append(
        commands,
        expected,
        "L 3 1",
        f"L {status} {count if status == OK else 0} "
        f"{signal_epoch if status == OK else 0}",
    )
    status, invalid_epoch = oracle.observe_event(EVENT_LIMIT)
    append(
        commands,
        expected,
        f"O {EVENT_LIMIT}",
        f"O {status} {invalid_epoch}",
    )
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
        "I\nK 10\n",
        "I\nK 11\n",
        "I\nC\nY\nK 12\n",
        "I\nK 13\n",
        "I\nC\nK 14\n",
        "I\nC\nY\nK 15\n",
        "I\nK 16\n",
        "I\nK 17\n",
        "I\nK 18\n",
        "I\nK 19\n",
        "I\nK 20\n",
        "I\nK 21\n",
        "I\nK 22\n",
        "I\nK 23\n",
        "I\nK 24\n",
        "I\nK 25\n",
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
    priority_event_boundaries(commands, expected)
    event_boundaries(commands, expected)

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
                print(f" protocol {index + 1}: {commands[index]}", file=sys.stderr)
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

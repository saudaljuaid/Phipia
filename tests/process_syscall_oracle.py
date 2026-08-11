#!/usr/bin/env python3
"""Randomized independent model for process, syscall, and user-range cores."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass

MASK64 = (1 << 64) - 1
PROCESS_LIMIT = 16
TASK_LIMIT = 65_535
USER_MAX = 0x00007FFFFFFFFFFF
PAGE_SIZE = 4096

OK = 0
NULL = 1
ALREADY = 2
NOT_INIT = 3
POISONED = 4
INVALID_HANDLE = 5
STALE = 6
NO_SLOTS = 7
GEN_EXHAUSTED = 8
INVALID_PARENT = 9
INVALID_SPACE = 10
INVALID_STATE = 11
TASK_LIMIT_STATUS = 12
NO_TASKS = 13
EPOCH_EXHAUSTED = 14
WOULD_BLOCK = 15
EVENT_CHANGED = 16
NOT_PARENT = 17
PROCESS_BUSY = 18

FREE, RUNNABLE, BLOCKED, EXITED, DYING = range(5)


@dataclass
class Slot:
    generation: int = 0
    state: int = FREE
    parent: int = 0
    space: int = 0
    epoch: int = 0
    tasks: int = 0
    children: int = 0
    exit_status: int = 0


class Model:
    def __init__(self) -> None:
        self.initialized = False
        self.slots = [Slot() for _ in range(PROCESS_LIMIT)]

    def clear(self) -> None:
        self.__init__()

    def initialize(self) -> int:
        if self.initialized:
            return ALREADY
        self.clear()
        self.initialized = True
        return OK

    @staticmethod
    def make_handle(index: int, generation: int) -> int:
        return generation << 32 | index + 1

    def resolve(self, handle: int) -> tuple[int, Slot | None]:
        if not self.initialized:
            return NOT_INIT, None
        encoded = handle & 0xFFFFFFFF
        generation = handle >> 32
        if not 1 <= encoded <= PROCESS_LIMIT or generation == 0:
            return INVALID_HANDLE, None
        slot = self.slots[encoded - 1]
        if slot.state == FREE or slot.generation != generation:
            return STALE, None
        return OK, slot

    def create(self, parent: int, space: int) -> tuple[int, int]:
        if not self.initialized:
            return NOT_INIT, 0
        if not address_space_syntax(space):
            return INVALID_SPACE, 0
        parent_slot = None
        if parent:
            status, parent_slot = self.resolve(parent)
            if status != OK or parent_slot is None or parent_slot.state not in (
                RUNNABLE,
                BLOCKED,
            ):
                return INVALID_PARENT, 0
        exhausted = False
        for index, slot in enumerate(self.slots):
            if slot.state != FREE:
                continue
            if slot.generation == 0xFFFFFFFF:
                exhausted = True
                continue
            slot.generation += 1
            slot.state = RUNNABLE
            slot.parent = parent
            slot.space = space
            slot.epoch = 1
            slot.tasks = slot.children = slot.exit_status = 0
            if parent_slot is not None:
                parent_slot.children += 1
            return OK, self.make_handle(index, slot.generation)
        return (GEN_EXHAUSTED if exhausted else NO_SLOTS), 0

    def unary(self, op: str, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK or slot is None:
            return status
        if op == "A":
            if slot.state not in (RUNNABLE, BLOCKED):
                return INVALID_STATE
            if slot.tasks == TASK_LIMIT:
                return TASK_LIMIT_STATUS
            slot.tasks += 1
        elif op == "D":
            if slot.state == DYING:
                return INVALID_STATE
            if slot.tasks == 0:
                return NO_TASKS
            slot.tasks -= 1
            if slot.state == BLOCKED and slot.tasks == 0:
                slot.state = RUNNABLE
        elif op == "B":
            if slot.state != RUNNABLE:
                return INVALID_STATE
            if slot.tasks == 0:
                return NO_TASKS
            slot.state = BLOCKED
        elif op == "R":
            if slot.state != BLOCKED:
                return INVALID_STATE
            slot.state = RUNNABLE
        elif op == "Y":
            if slot.state != EXITED:
                return INVALID_STATE
            if slot.tasks or slot.children:
                return PROCESS_BUSY
            slot.state = DYING
        elif op == "K":
            if slot.state != DYING:
                return INVALID_STATE
            if slot.tasks or slot.children:
                return PROCESS_BUSY
            if slot.parent:
                _, parent = self.resolve(slot.parent)
                assert parent is not None and parent.children
                parent.children -= 1
            generation = slot.generation
            slot.__dict__.update(Slot().__dict__)
            slot.generation = generation
        return OK

    def exit(self, handle: int, exit_status: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK or slot is None:
            return status
        if slot.state not in (RUNNABLE, BLOCKED):
            return INVALID_STATE
        if slot.epoch == MASK64:
            return EPOCH_EXHAUSTED
        slot.epoch += 1
        slot.exit_status = exit_status
        slot.state = EXITED
        return OK

    def relation(self, waiter: int, child: int) -> tuple[int, Slot | None]:
        status, waiter_slot = self.resolve(waiter)
        if status != OK or waiter_slot is None:
            return status, None
        if waiter_slot.state not in (RUNNABLE, BLOCKED):
            return INVALID_STATE, None
        status, child_slot = self.resolve(child)
        if status != OK or child_slot is None:
            return status, None
        if child_slot.parent != waiter:
            return NOT_PARENT, None
        return OK, child_slot

    def observe(self, waiter: int, child: int) -> tuple[int, int, int, int]:
        status, slot = self.relation(waiter, child)
        if status != OK or slot is None:
            return status, 0, 0, 0
        if slot.state in (EXITED, DYING):
            return OK, child, slot.epoch, slot.exit_status
        return WOULD_BLOCK, child, slot.epoch, 0

    def complete(self, waiter: int, child: int, epoch: int) -> tuple[int, int]:
        if epoch == 0:
            return EVENT_CHANGED, 0
        status, slot = self.relation(waiter, child)
        if status != OK or slot is None:
            return status, 0
        if slot.epoch == epoch:
            if slot.state in (EXITED, DYING):
                return OK, slot.exit_status
            return WOULD_BLOCK, 0
        if slot.state not in (EXITED, DYING):
            return EVENT_CHANGED, 0
        return OK, slot.exit_status

    def snapshot(self, handle: int) -> str:
        status, slot = self.resolve(handle)
        if status != OK or slot is None:
            return f"G {status} 0 0 0 0 0 0 0 0"
        return (
            f"G 0 {slot.state} {slot.generation} {slot.parent} {slot.space} "
            f"{slot.epoch} {slot.tasks} {slot.children} {slot.exit_status}"
        )

    def live_handles(self) -> list[int]:
        return [
            self.make_handle(index, slot.generation)
            for index, slot in enumerate(self.slots)
            if slot.state != FREE
        ]


def address_space_syntax(handle: int) -> bool:
    encoded = handle & 0xFFFFFFFF
    return 1 <= encoded <= 16 and handle >> 32 != 0


def syscall_expected(number: int, args: list[int]) -> str:
    status = 0
    action = exit_status = descriptor = event = address = length = epoch = process = wake = 0
    if number >= 7:
        status = 2
    elif number == 0:
        if any(args[1:]):
            status = 3
        else:
            low, high = args[0] & 0xFFFFFFFF, args[0] >> 32
            if high != 0 and not (high == 0xFFFFFFFF and low & 0x80000000):
                status = 4
            else:
                action = 1
                exit_status = low if low <= 0x7FFFFFFF else low - (1 << 32)
    elif number in (1, 2):
        if any(args):
            status = 3
        else:
            action = number + 1
    elif number == 3:
        if any(args[3:]):
            status = 3
        elif args[0] not in (1, 2):
            status = 5
        elif args[2] > 4096:
            status = 7
        elif args[2] and not (
            0x1000 <= args[1] <= USER_MAX and args[2] - 1 <= USER_MAX - args[1]
        ):
            status = 6
        else:
            action, descriptor, address, length = 4, args[0], args[1], args[2]
    elif number == 4:
        if any(args[2:]):
            status = 3
        elif args[0] >= 8:
            status = 8
        elif args[1] == 0:
            status = 9
        else:
            action, event, epoch = 5, args[0], args[1]
    elif number == 5:
        if any(args[2:]):
            status = 3
        elif args[0] >= 8:
            status = 8
        elif args[1] > 1:
            status = 10
        else:
            action, event, wake = 6, args[0], args[1]
    elif number == 6:
        if any(args[1:]):
            status = 3
        elif not address_space_syntax(args[0]):
            status = 11
        else:
            action, process = 7, args[0]
    return (
        f"S {status} {action} {exit_status} {descriptor} {event} {address} "
        f"{length} {epoch} {process} {wake}"
    )


def mapping_valid(mapping: tuple[int, int, int, int, int]) -> bool:
    page, permissions, user, writable, executable = mapping
    return (
        page <= USER_MAX // PAGE_SIZE
        and permissions & ~15 == 0
        and not (permissions & 2 and permissions & 4)
        and bool(user) == bool(permissions & 8)
        and bool(writable) == bool(permissions & 2)
        and bool(executable) == bool(permissions & 4)
    )


def usercopy_expected(
    address: int,
    length: int,
    access: int,
    mappings: list[tuple[int, int, int, int, int]],
) -> str:
    first = last = failed = pages = crossed = 0
    if access not in (0, 1):
        status = 2
    elif length == 0:
        status = 0
    elif length > 65536:
        status = 6
    elif length - 1 > MASK64 - address:
        status = 5
    elif address < 4096:
        status = 3
    elif address > USER_MAX or address + length - 1 > USER_MAX:
        status = 4
    else:
        last_address = address + length - 1
        first, last = address // PAGE_SIZE, last_address // PAGE_SIZE
        pages, crossed = last - first + 1, int(first != last)
        if pages > 16:
            status = 7
            pages = 0
        elif len(mappings) > 32:
            status = 8
        else:
            status = 0
            seen: set[int] = set()
            for mapping in mappings:
                if not mapping_valid(mapping):
                    status, failed = 9, mapping[0]
                    break
                if mapping[0] in seen:
                    status, failed = 10, mapping[0]
                    break
                seen.add(mapping[0])
            if status == 0:
                by_page = {mapping[0]: mapping for mapping in mappings}
                for page in range(first, last + 1):
                    mapping = by_page.get(page)
                    if mapping is None:
                        status, failed = 11, page
                        break
                    permissions = mapping[1]
                    if not mapping[2]:
                        status, failed = 12, page
                        break
                    if not permissions & 1:
                        status, failed = 13, page
                        break
                    if access == 1 and not mapping[3]:
                        status, failed = 14, page
                        break
    return f"U {status} {first} {last} {failed} {pages} {crossed}"


def frame_expected(kind: int, values: list[int]) -> str:
    rip, cs, flags, rsp, ss = values
    if cs != 0x33 or cs & 3 != 3:
        status = 2
    elif ss != 0x2B or ss & 3 != 3:
        status = 3
    elif not 0x1000 <= rip <= USER_MAX:
        status = 4
    elif not 0x1000 <= rsp <= USER_MAX:
        status = 5
    elif not flags & 2 or not flags & 0x200 or flags & ~0xFD7:
        status = 6
    elif kind == 0 and flags != 0x202:
        status = 6
    elif kind == 0 and rsp & 15:
        status = 5
    else:
        status = 0
    return f"J {status}"


def choose_handle(rng: random.Random, model: Model, history: list[int]) -> int:
    live = model.live_handles()
    choice = rng.randrange(10)
    if live and choice < 6:
        return rng.choice(live)
    if history and choice < 8:
        return rng.choice(history)
    return rng.choice([0, rng.getrandbits(64), rng.randrange(1, 24)])


def generate_process(
    rng: random.Random,
    model: Model,
    history: list[int],
) -> tuple[str, str]:
    choice = rng.randrange(100)
    if choice < 3:
        model.clear()
        return "Z", "Z"
    if choice < 7:
        return "I", f"I {model.initialize()}"
    if choice < 27:
        parent = 0 if rng.randrange(4) == 0 else choose_handle(rng, model, history)
        space = (
            rng.randrange(1, 8) << 32 | rng.randrange(1, 17)
            if rng.randrange(5)
            else rng.choice([0, 17, rng.getrandbits(64)])
        )
        status, handle = model.create(parent, space)
        if handle:
            history.append(handle)
        return f"C {parent} {space}", f"C {status} {handle}"
    if choice < 70:
        op = rng.choice("AADDBBRRYYK")
        handle = choose_handle(rng, model, history)
        status = model.unary(op, handle)
        return f"{op} {handle}", f"{op} {status}"
    if choice < 80:
        handle = choose_handle(rng, model, history)
        code = rng.randrange(-(1 << 31), 1 << 31)
        status = model.exit(handle, code)
        return f"E {handle} {code}", f"E {status}"
    if choice < 88:
        waiter = choose_handle(rng, model, history)
        child = choose_handle(rng, model, history)
        status, process, epoch, code = model.observe(waiter, child)
        return (
            f"O {waiter} {child}",
            f"O {status} {process} {epoch} {code}",
        )
    if choice < 95:
        waiter = choose_handle(rng, model, history)
        child = choose_handle(rng, model, history)
        _, child_slot = model.resolve(child)
        epoch = (
            rng.choice([0, child_slot.epoch, max(1, child_slot.epoch - 1)])
            if child_slot is not None
            else rng.randrange(0, 4)
        )
        status, code = model.complete(waiter, child, epoch)
        return f"W {waiter} {child} {epoch}", f"W {status} {code}"
    if choice < 99:
        handle = choose_handle(rng, model, history)
        return f"G {handle}", model.snapshot(handle)
    return (
        "X",
        f"X {OK if model.initialized else NOT_INIT} "
        f"{len(model.live_handles())}",
    )


def generate_syscall(rng: random.Random) -> tuple[str, str]:
    number = rng.randrange(10)
    args = [0] * 6
    if number == 0:
        args[0] = rng.choice([0, 1, 0xFFFFFFFF, MASK64, rng.getrandbits(64)])
    elif number in (1, 2):
        pass
    elif number == 3:
        args[:3] = [
            rng.randrange(0, 4),
            rng.choice([0, 0x1000, USER_MAX, rng.getrandbits(64)]),
            rng.choice([0, 1, 4096, 4097, rng.getrandbits(16)]),
        ]
    elif number == 4:
        args[:2] = [rng.randrange(0, 12), rng.randrange(0, 5)]
    elif number == 5:
        args[:2] = [rng.randrange(0, 12), rng.randrange(0, 4)]
    elif number == 6:
        args[0] = rng.choice([0, 1, 1 << 32 | 1, rng.getrandbits(64)])
    if rng.randrange(5) == 0:
        args[rng.randrange(6)] = rng.getrandbits(64)
    command = "S " + " ".join(map(str, [number, *args]))
    return command, syscall_expected(number, args)


def generate_usercopy(rng: random.Random) -> tuple[str, str]:
    address = rng.choice(
        [0, 4095, 4096, 8191, USER_MAX, MASK64, rng.randrange(4096, 1 << 20)]
    )
    length = rng.choice([0, 1, 4096, 4097, 65536, 65537, rng.randrange(0, 70000)])
    access = rng.randrange(0, 4)
    mappings: list[tuple[int, int, int, int, int]] = []
    if length and address <= MASK64 and length - 1 <= MASK64 - address:
        first = address // PAGE_SIZE
        last = (address + length - 1) // PAGE_SIZE
        if last - first < 18:
            for page in range(first, last + 1):
                permissions = rng.choice([1, 3, 5, 9, 11, 0, 15, 16])
                mappings.append(
                    (
                        page,
                        permissions,
                        int(bool(permissions & 8)),
                        int(bool(permissions & 2)),
                        int(bool(permissions & 4)),
                    )
                )
    if rng.randrange(5) == 0 and mappings:
        mappings.append(rng.choice(mappings))
    if rng.randrange(20) == 0:
        mappings = [(index, 9, 1, 0, 0) for index in range(33)]
    if rng.randrange(8) == 0 and mappings:
        index = rng.randrange(len(mappings))
        page, permissions, user, writable, executable = mappings[index]
        mappings[index] = (page, permissions, user ^ 1, writable, executable)
    fields = " ".join(
        " ".join(map(str, mapping)) for mapping in mappings
    )
    command = f"U {address} {length} {access} {len(mappings)}"
    if fields:
        command += " " + fields
    return command, usercopy_expected(address, length, access, mappings)


def generate_frame(rng: random.Random) -> tuple[str, str]:
    kind = rng.randrange(2)
    values = [
        rng.choice([0, 0x1000, 0x400000, USER_MAX, 1 << 63]),
        rng.choice([0x33, 0x30, 8, rng.getrandbits(16)]),
        rng.choice([0x202, 2, 0x203, 0x10002, rng.getrandbits(24)]),
        rng.choice([0, 0x1000, 0x800000, 0x800008, USER_MAX]),
        rng.choice([0x2B, 0x28, 0x10, rng.getrandbits(16)]),
    ]
    command = "J " + " ".join(map(str, [kind, *values]))
    return command, frame_expected(kind, values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--operations", type=int, default=300_000)
    parser.add_argument("--seed", type=int, default=0x50524F43)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    model = Model()
    history: list[int] = []
    commands = ["H", "I"]
    expected = ["H 1", "I 0"]
    model.initialize()

    for _ in range(args.operations):
        category = rng.randrange(100)
        if category < 60:
            command, output = generate_process(rng, model, history)
        elif category < 80:
            command, output = generate_syscall(rng)
        elif category < 95:
            command, output = generate_usercopy(rng)
        elif category < 99:
            command, output = generate_frame(rng)
        else:
            cs = rng.getrandbits(16)
            command, output = f"F {cs}", f"F {1 if cs & 3 == 3 else 0}"
        commands.append(command)
        expected.append(output)

    completed = subprocess.run(
        [args.runner],
        input="\n".join(commands) + "\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr)
        print(f"runner exited {completed.returncode}", file=sys.stderr)
        return 1
    actual = completed.stdout.splitlines()
    if len(actual) != len(expected):
        print(f"line count: expected {len(expected)}, got {len(actual)}", file=sys.stderr)
        return 1
    for index, (wanted, got) in enumerate(zip(expected, actual, strict=True)):
        if wanted != got:
            print(f"mismatch at operation {index}", file=sys.stderr)
            print(f"command:  {commands[index]}", file=sys.stderr)
            print(f"expected: {wanted}", file=sys.stderr)
            print(f"actual:   {got}", file=sys.stderr)
            return 1

    print(
        f"process/syscall/usercopy oracle passed: {args.operations} randomized "
        f"operations (seed {args.seed})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

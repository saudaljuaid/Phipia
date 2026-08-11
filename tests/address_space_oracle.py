#!/usr/bin/env python3
"""Randomized independent model for the bounded address-space core."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass, field

SPACE_LIMIT = 16
PAGE_LIMIT = 32
TABLE_LIMIT = 16
CPU_LIMIT = 16
REF_LIMIT = 65_535
MAX_FRAME = 0x000000FFFFFFFFFF
LOW_MAX = 0x00000007FFFFFFFF
HIGH_MIN = 0x000FFFF800000000
VPAGE_MAX = 0x000FFFFFFFFFFFFF

OK = 0
NULL_ARGUMENT = 1
ALREADY_INITIALIZED = 2
NOT_INITIALIZED = 3
POISONED = 4
INVALID_HANDLE = 5
STALE_HANDLE = 6
NO_SLOTS = 7
GENERATION_EXHAUSTED = 8
INVALID_STATE = 9
INVALID_FRAME = 10
INVALID_VIRTUAL_PAGE = 11
INVALID_PERMISSIONS = 12
MAPPING_EXISTS = 13
MAPPING_NOT_FOUND = 14
MAPPING_LIMIT = 15
TABLE_FRAME_EXISTS = 16
TABLE_FRAME_LIMIT = 17
REFERENCE_OVERFLOW = 18
REFERENCE_UNDERFLOW = 19
INVALID_CPU = 20
CPU_ACTIVE = 21
CPU_INACTIVE = 22
SHOOTDOWN_REQUIRED = 23
SHOOTDOWN_IN_PROGRESS = 24
NO_SHOOTDOWN_REQUIRED = 25
SHOOTDOWN_NOT_IN_PROGRESS = 26
SHOOTDOWN_STALE_GENERATION = 27
CPU_NOT_TARGETED = 28
DUPLICATE_ACK = 29
SHOOTDOWN_INCOMPLETE = 30
SPACE_BUSY = 31

FREE = 0
BUILDING = 1
PUBLISHED = 2
DYING = 3

FNV_OFFSET = 1_469_598_103_934_665_603
FNV_PRIME = 1_099_511_628_211
MASK64 = (1 << 64) - 1


@dataclass
class Mapping:
    virtual_page: int
    physical_frame: int
    permissions: int


@dataclass
class Slot:
    generation: int = 0
    state: int = FREE
    tlb_generation: int = 0
    mappings: list[Mapping] = field(default_factory=list)
    table_frames: list[int] = field(default_factory=list)
    references: int = 0
    active: int = 0
    required: int = 0
    target: int = 0
    ack: int = 0
    pending: bool = False

    @property
    def cr3(self) -> int:
        return self.table_frames[0] << 12 if self.table_frames else 0

    def release(self) -> list[int]:
        frames = self.table_frames[:]
        generation = self.generation
        self.__dict__.update(Slot().__dict__)
        self.generation = generation
        return frames


class Model:
    def __init__(self) -> None:
        self.initialized = False
        self.poisoned = False
        self.slots = [Slot() for _ in range(SPACE_LIMIT)]

    def clear(self) -> None:
        self.__init__()

    @property
    def occupied(self) -> int:
        return sum(slot.state != FREE for slot in self.slots)

    def frame_owned(self, frame: int) -> bool:
        return any(
            frame in slot.table_frames
            for slot in self.slots
            if slot.state != FREE
        )

    def frame_user_mapped(self, frame: int) -> bool:
        return any(
            mapping.physical_frame == frame and mapping.permissions & 8
            for slot in self.slots
            if slot.state != FREE
            for mapping in slot.mappings
        )

    def initialize(self) -> int:
        if self.initialized:
            return ALREADY_INITIALIZED
        self.clear()
        self.initialized = True
        return OK

    def resolve(self, handle: int) -> tuple[int, Slot | None]:
        if not self.initialized:
            return NOT_INITIALIZED, None
        if self.poisoned:
            return POISONED, None
        encoded = handle & 0xFFFFFFFF
        generation = handle >> 32
        if not (1 <= encoded <= SPACE_LIMIT) or generation == 0:
            return INVALID_HANDLE, None
        slot = self.slots[encoded - 1]
        if slot.state == FREE or slot.generation != generation:
            return STALE_HANDLE, None
        return OK, slot

    def create(self, frame: int) -> tuple[int, int]:
        if not self.initialized:
            return NOT_INITIALIZED, 0
        if self.poisoned:
            return POISONED, 0
        if not table_frame_valid(frame):
            return INVALID_FRAME, 0
        if self.frame_owned(frame):
            return TABLE_FRAME_EXISTS, 0
        if self.frame_user_mapped(frame):
            return INVALID_FRAME, 0
        saw_exhausted = False
        for index, slot in enumerate(self.slots):
            if slot.state != FREE:
                continue
            if slot.generation == 0xFFFFFFFF:
                saw_exhausted = True
                continue
            slot.generation += 1
            slot.state = BUILDING
            slot.tlb_generation = 1
            slot.table_frames = [frame]
            return OK, (slot.generation << 32) | (index + 1)
        return (GENERATION_EXHAUSTED if saw_exhausted else NO_SLOTS), 0

    def own(self, handle: int, frame: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state != BUILDING:
            return INVALID_STATE
        if not table_frame_valid(frame):
            return INVALID_FRAME
        if self.frame_owned(frame):
            return TABLE_FRAME_EXISTS
        if self.frame_user_mapped(frame):
            return INVALID_FRAME
        if len(slot.table_frames) == TABLE_LIMIT:
            return TABLE_FRAME_LIMIT
        slot.table_frames.append(frame)
        return OK

    @staticmethod
    def mutation_gate(slot: Slot) -> int:
        if slot.state not in (BUILDING, PUBLISHED):
            return INVALID_STATE
        if slot.pending:
            return SHOOTDOWN_IN_PROGRESS
        if slot.tlb_generation == 0xFFFFFFFF:
            return GENERATION_EXHAUSTED
        return OK

    @staticmethod
    def changed(slot: Slot) -> None:
        slot.tlb_generation += 1
        if slot.state == PUBLISHED:
            slot.required |= slot.active

    def map(self, handle: int, page: int, frame: int, permissions: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        status = self.mutation_gate(slot)
        if status != OK:
            return status
        if not virtual_page_valid(page):
            return INVALID_VIRTUAL_PAGE
        if not (0 <= frame <= MAX_FRAME):
            return INVALID_FRAME
        if not permissions_valid(page, permissions):
            return INVALID_PERMISSIONS
        if permissions & 8 and self.frame_owned(frame):
            return INVALID_PERMISSIONS
        if any(mapping.virtual_page == page for mapping in slot.mappings):
            return MAPPING_EXISTS
        if len(slot.mappings) == PAGE_LIMIT:
            return MAPPING_LIMIT
        slot.mappings.append(Mapping(page, frame, permissions))
        self.changed(slot)
        return OK

    def unmap(self, handle: int, page: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        status = self.mutation_gate(slot)
        if status != OK:
            return status
        if not virtual_page_valid(page):
            return INVALID_VIRTUAL_PAGE
        for index, mapping in enumerate(slot.mappings):
            if mapping.virtual_page == page:
                slot.mappings.pop(index)
                self.changed(slot)
                return OK
        return MAPPING_NOT_FOUND

    def repermission(self, handle: int, page: int, permissions: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        status = self.mutation_gate(slot)
        if status != OK:
            return status
        if not virtual_page_valid(page):
            return INVALID_VIRTUAL_PAGE
        if not permissions_valid(page, permissions):
            return INVALID_PERMISSIONS
        for mapping in slot.mappings:
            if mapping.virtual_page == page:
                if permissions & 8 and self.frame_owned(mapping.physical_frame):
                    return INVALID_PERMISSIONS
                if mapping.permissions != permissions:
                    mapping.permissions = permissions
                    self.changed(slot)
                return OK
        return MAPPING_NOT_FOUND

    def publish(self, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state != BUILDING:
            return INVALID_STATE
        slot.state = PUBLISHED
        return OK

    def abort(self, handle: int) -> tuple[int, list[int]]:
        status, slot = self.resolve(handle)
        if status != OK:
            return status, []
        assert slot is not None
        if slot.state != BUILDING:
            return INVALID_STATE, []
        return OK, slot.release()

    def retain(self, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state != PUBLISHED:
            return INVALID_STATE
        if slot.references >= REF_LIMIT:
            return REFERENCE_OVERFLOW
        slot.references += 1
        return OK

    def release(self, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state not in (PUBLISHED, DYING):
            return INVALID_STATE
        if slot.references == 0:
            return REFERENCE_UNDERFLOW
        slot.references -= 1
        return OK

    def activate(self, handle: int, cpu: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state != PUBLISHED:
            return INVALID_STATE
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        bit = 1 << cpu
        if slot.active & bit:
            return CPU_ACTIVE
        if slot.pending:
            return SHOOTDOWN_IN_PROGRESS
        if slot.required:
            return SHOOTDOWN_REQUIRED
        slot.active |= bit
        return OK

    def deactivate(self, handle: int, cpu: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state not in (PUBLISHED, DYING):
            return INVALID_STATE
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        bit = 1 << cpu
        if not slot.active & bit:
            return CPU_INACTIVE
        slot.active &= ~bit
        slot.required &= ~bit
        slot.target &= ~bit
        slot.ack &= ~bit
        return OK

    def begin(self, handle: int) -> tuple[int, int, int]:
        status, slot = self.resolve(handle)
        if status != OK:
            return status, 0, 0
        assert slot is not None
        if slot.state not in (PUBLISHED, DYING):
            return INVALID_STATE, 0, 0
        if slot.pending:
            return SHOOTDOWN_IN_PROGRESS, 0, 0
        if not slot.required:
            return NO_SHOOTDOWN_REQUIRED, 0, 0
        slot.target = slot.required
        slot.ack = 0
        slot.pending = True
        return OK, slot.tlb_generation, slot.target

    def ack_shootdown(self, handle: int, cpu: int, generation: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state not in (PUBLISHED, DYING):
            return INVALID_STATE
        if not slot.pending:
            return SHOOTDOWN_NOT_IN_PROGRESS
        if cpu >= CPU_LIMIT:
            return INVALID_CPU
        bit = 1 << cpu
        if generation != slot.tlb_generation:
            return SHOOTDOWN_STALE_GENERATION
        if not slot.target & bit:
            return CPU_NOT_TARGETED
        if slot.ack & bit:
            return DUPLICATE_ACK
        slot.ack |= bit
        return OK

    def complete(self, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state not in (PUBLISHED, DYING):
            return INVALID_STATE
        if not slot.pending:
            return SHOOTDOWN_NOT_IN_PROGRESS
        if slot.ack != slot.target:
            return SHOOTDOWN_INCOMPLETE
        slot.required &= ~slot.target
        slot.target = 0
        slot.ack = 0
        slot.pending = False
        return OK

    def destroy(self, handle: int) -> int:
        status, slot = self.resolve(handle)
        if status != OK:
            return status
        assert slot is not None
        if slot.state != PUBLISHED:
            return INVALID_STATE
        slot.state = DYING
        return OK

    def reap(self, handle: int) -> tuple[int, list[int]]:
        status, slot = self.resolve(handle)
        if status != OK:
            return status, []
        assert slot is not None
        if slot.state != DYING:
            return INVALID_STATE, []
        if (
            slot.active
            or slot.references
            or slot.required
            or slot.target
            or slot.ack
            or slot.pending
        ):
            return SPACE_BUSY, []
        return OK, slot.release()


def table_frame_valid(frame: int) -> bool:
    return 0 < frame <= MAX_FRAME


def virtual_page_valid(page: int) -> bool:
    return 0 <= page <= LOW_MAX or HIGH_MIN <= page <= VPAGE_MAX


def permissions_valid(page: int, permissions: int) -> bool:
    return (
        permissions & ~0xF == 0
        and permissions & 1 != 0
        and not (permissions & 2 and permissions & 4)
        and not (permissions & 8 and page > LOW_MAX)
    )


def mix(digest: int, value: int) -> int:
    return ((digest ^ value) * FNV_PRIME) & MASK64


def state_digest(model: Model) -> int:
    digest = FNV_OFFSET
    digest = mix(digest, int(model.initialized))
    digest = mix(digest, int(model.poisoned))
    digest = mix(digest, model.occupied)
    for slot in model.slots:
        values = (
            slot.generation,
            slot.state,
            slot.tlb_generation,
            len(slot.mappings),
            len(slot.table_frames),
            slot.references,
            slot.active,
            slot.required,
            slot.target,
            slot.ack,
            int(slot.pending),
            slot.cr3,
        )
        for value in values:
            digest = mix(digest, value)
        for index in range(TABLE_LIMIT):
            digest = mix(
                digest,
                slot.table_frames[index] if index < len(slot.table_frames) else 0,
            )
        for index in range(PAGE_LIMIT):
            if index < len(slot.mappings):
                mapping = slot.mappings[index]
                mapping_values = (
                    mapping.virtual_page,
                    mapping.physical_frame,
                    mapping.permissions,
                )
            else:
                mapping_values = (0, 0, 0)
            for value in mapping_values:
                digest = mix(digest, value)
    return digest


def bundle_digest(frames: list[int]) -> int:
    digest = mix(FNV_OFFSET, len(frames))
    for frame in frames:
        digest = mix(digest, frame)
    return digest


class Harness:
    def __init__(self) -> None:
        self.model = Model()
        self.commands: list[str] = []
        self.expected: list[str] = []
        self.handles: list[int] = []
        self.stale: list[int] = []

    def emit(self, command: str) -> None:
        parts = command.split()
        op = parts[0]
        args = [int(value) for value in parts[1:]]
        model = self.model

        if op == "Z":
            model.clear()
            output = "Z"
        elif op == "I":
            output = f"I {model.initialize()}"
        elif op == "C":
            status, handle = model.create(args[0])
            if status == OK:
                self.handles.append(handle)
            output = f"C {status} {handle}"
        elif op == "T":
            output = f"T {model.own(args[0], args[1])}"
        elif op == "M":
            output = f"M {model.map(args[0], args[1], args[2], args[3])}"
        elif op == "U":
            output = f"U {model.unmap(args[0], args[1])}"
        elif op == "P":
            output = f"P {model.repermission(args[0], args[1], args[2])}"
        elif op == "B":
            output = f"B {model.publish(args[0])}"
        elif op in ("A", "E"):
            status, frames = (
                model.abort(args[0]) if op == "A" else model.reap(args[0])
            )
            if status == OK:
                self.stale.append(args[0])
            output = f"{op} {status} {len(frames)} {bundle_digest(frames)}"
        elif op == "R":
            output = f"R {model.retain(args[0])}"
        elif op == "L":
            output = f"L {model.release(args[0])}"
        elif op == "V":
            output = f"V {model.activate(args[0], args[1])}"
        elif op == "D":
            output = f"D {model.deactivate(args[0], args[1])}"
        elif op == "S":
            status, generation, target = model.begin(args[0])
            output = f"S {status} {generation} {target}"
        elif op == "K":
            output = f"K {model.ack_shootdown(args[0], args[1], args[2])}"
        elif op == "Q":
            output = f"Q {model.complete(args[0])}"
        elif op == "Y":
            output = f"Y {model.destroy(args[0])}"
        elif op == "G":
            status, slot = model.resolve(args[0])
            if status == OK:
                assert slot is not None
                values = (
                    slot.state,
                    slot.generation,
                    slot.cr3,
                    len(slot.table_frames),
                    len(slot.mappings),
                    slot.references,
                    slot.tlb_generation,
                    slot.active,
                    slot.required,
                    slot.target,
                    slot.ack,
                    int(slot.pending),
                )
            else:
                values = (0,) * 12
            output = "G " + " ".join(str(value) for value in (status, *values))
        elif op == "N":
            status, slot = model.resolve(args[0])
            if status == OK and slot is not None and args[1] < len(slot.mappings):
                mapping = slot.mappings[args[1]]
                values = (
                    mapping.virtual_page,
                    mapping.physical_frame,
                    mapping.permissions,
                    int(bool(mapping.permissions & 8)),
                    int(bool(mapping.permissions & 2)),
                    int(bool(mapping.permissions & 4)),
                )
            else:
                if status == OK:
                    status = MAPPING_NOT_FOUND
                values = (0,) * 6
            output = "N " + " ".join(str(value) for value in (status, *values))
        elif op == "X":
            status = OK if model.initialized else NOT_INITIALIZED
            output = f"X {status} {state_digest(model)}"
        elif op == "H":
            output = "H 1"
        else:
            raise AssertionError(op)

        self.commands.append(command)
        self.expected.append(output)

    def valid_handle(self, rng: random.Random) -> int:
        live = [
            (slot.generation << 32) | index
            for index, slot in enumerate(self.model.slots, 1)
            if slot.state != FREE
        ]
        if live and rng.random() < 0.82:
            return rng.choice(live)
        if self.stale and rng.random() < 0.6:
            return rng.choice(self.stale)
        return rng.choice([0, 17, 1, rng.getrandbits(64)])


def directed(harness: Harness) -> None:
    e = harness.emit
    e("X")
    e("I")
    e("I")
    e("H")
    e("C 0")
    e("C 1")
    handle = harness.handles[-1]
    e(f"T {handle} 1")
    e(f"T {handle} 0")
    e(f"T {handle} 2")
    e(f"M {handle} {LOW_MAX + 1} 3 1")
    e(f"M {handle} 1 {MAX_FRAME + 1} 1")
    e(f"M {handle} 1 3 0")
    e(f"M {handle} 1 3 7")
    e(f"M {handle} {HIGH_MIN} 3 9")
    e(f"M {handle} 1 3 11")
    e(f"M {handle} {HIGH_MIN} 4 5")
    e(f"M {handle} 1 5 1")
    e(f"N {handle} 0")
    e(f"N {handle} 9")
    e(f"P {handle} 9 1")
    e(f"B {handle}")
    e(f"B {handle}")
    e(f"L {handle}")
    e(f"V {handle} 16")
    e(f"V {handle} 0")
    e(f"V {handle} 0")
    e(f"R {handle}")
    e(f"M {handle} 2 8 3")
    e(f"V {handle} 1")
    e(f"S {handle}")
    generation = harness.model.resolve(handle)[1].tlb_generation  # type: ignore[union-attr]
    e(f"M {handle} 3 9 1")
    e(f"Q {handle}")
    e(f"K {handle} 16 {generation}")
    e(f"K {handle} 0 {generation - 1}")
    e(f"K {handle} 1 {generation}")
    e(f"K {handle} 0 {generation}")
    e(f"K {handle} 0 {generation}")
    e(f"Q {handle}")
    e(f"V {handle} 1")
    e(f"P {handle} 1 9")
    e(f"Y {handle}")
    e(f"E {handle}")
    e(f"S {handle}")
    generation = harness.model.resolve(handle)[1].tlb_generation  # type: ignore[union-attr]
    e(f"D {handle} 1")
    e(f"K {handle} 0 {generation}")
    e(f"Q {handle}")
    e(f"D {handle} 0")
    e(f"D {handle} 0")
    e(f"L {handle}")
    e(f"E {handle}")
    e(f"G {handle}")
    e("C 10")
    second = harness.handles[-1]
    e(f"A {second}")
    e("X")


def randomized(harness: Harness, seed: int, operations: int) -> None:
    rng = random.Random(seed)
    harness.emit("Z")
    harness.emit("I")
    for operation_index in range(operations):
        handle = harness.valid_handle(rng)
        choice = rng.randrange(100)
        if choice < 8:
            frame = rng.choice([0, MAX_FRAME + 1, rng.randrange(1, 1 << 28)])
            harness.emit(f"C {frame}")
        elif choice < 13:
            frame = rng.choice([0, MAX_FRAME + 1, rng.randrange(1, 1 << 28)])
            harness.emit(f"T {handle} {frame}")
        elif choice < 27:
            page = random_page(rng, handle, harness.model)
            frame = rng.choice([MAX_FRAME + 1, rng.randrange(0, 1 << 28)])
            permissions = rng.choice([0, 1, 3, 5, 7, 9, 11, 13, 16])
            harness.emit(f"M {handle} {page} {frame} {permissions}")
        elif choice < 36:
            harness.emit(
                f"U {handle} {random_page(rng, handle, harness.model)}"
            )
        elif choice < 45:
            harness.emit(
                f"P {handle} {random_page(rng, handle, harness.model)} "
                f"{rng.choice([0, 1, 3, 5, 7, 9, 11, 13, 32])}"
            )
        elif choice < 49:
            harness.emit(f"B {handle}")
        elif choice < 53:
            harness.emit(f"A {handle}")
        elif choice < 58:
            harness.emit(f"R {handle}")
        elif choice < 63:
            harness.emit(f"L {handle}")
        elif choice < 69:
            harness.emit(f"V {handle} {rng.randrange(0, 19)}")
        elif choice < 75:
            harness.emit(f"D {handle} {rng.randrange(0, 19)}")
        elif choice < 80:
            harness.emit(f"S {handle}")
        elif choice < 85:
            status, slot = harness.model.resolve(handle)
            generation = (
                slot.tlb_generation
                if status == OK and slot is not None and rng.random() < 0.7
                else rng.randrange(0, 20)
            )
            harness.emit(
                f"K {handle} {rng.randrange(0, 19)} {generation}"
            )
        elif choice < 89:
            harness.emit(f"Q {handle}")
        elif choice < 93:
            harness.emit(f"Y {handle}")
        elif choice < 96:
            harness.emit(f"E {handle}")
        elif choice < 98:
            harness.emit(f"G {handle}")
        else:
            harness.emit(f"N {handle} {rng.randrange(0, PAGE_LIMIT + 4)}")

        if operation_index % 251 == 250:
            harness.emit("X")


def random_page(rng: random.Random, handle: int, model: Model) -> int:
    status, slot = model.resolve(handle)
    if status == OK and slot is not None and slot.mappings and rng.random() < 0.55:
        return rng.choice(slot.mappings).virtual_page
    return rng.choice(
        [
            rng.randrange(0, 1 << 20),
            HIGH_MIN + rng.randrange(0, 1 << 20),
            LOW_MAX + 1,
            HIGH_MIN - 1,
            VPAGE_MAX + 1,
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--operations", type=int, default=250_000)
    parser.add_argument(
        "--seeds",
        nargs="+",
        type=lambda value: int(value, 0),
        default=[0x5EED, 0xC0FFEE, 0xDECAFBAD, 0x13579BDF],
    )
    args = parser.parse_args()
    if args.operations < len(args.seeds):
        parser.error("--operations must be at least the number of seeds")

    harness = Harness()
    directed(harness)
    quotient, remainder = divmod(args.operations, len(args.seeds))
    for index, seed in enumerate(args.seeds):
        randomized(harness, seed, quotient + (index < remainder))
    harness.emit("X")

    process = subprocess.run(
        [args.runner],
        input="\n".join(harness.commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        print(process.stderr, file=sys.stderr)
        print(f"runner exited with {process.returncode}", file=sys.stderr)
        return 1

    actual = process.stdout.splitlines()
    if actual != harness.expected:
        limit = min(len(actual), len(harness.expected))
        mismatch = next(
            (
                index
                for index in range(limit)
                if actual[index] != harness.expected[index]
            ),
            limit,
        )
        start = max(0, mismatch - 3)
        end = min(max(len(actual), len(harness.expected)), mismatch + 4)
        for index in range(start, end):
            command = harness.commands[index] if index < len(harness.commands) else "<none>"
            expected = harness.expected[index] if index < len(harness.expected) else "<none>"
            observed = actual[index] if index < len(actual) else "<none>"
            print(
                f"[{index}] command={command!r} expected={expected!r} actual={observed!r}",
                file=sys.stderr,
            )
        return 1

    print(
        "address-space oracle passed: "
        f"{args.operations:,} randomized operations across {len(args.seeds)} fixed seeds; "
        f"{len(harness.commands):,} total checks"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

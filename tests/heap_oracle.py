#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the bounded C heap core with an independent list-model oracle."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass


OK = 0
ZERO_SIZE = 5
INVALID_ALIGNMENT = 6
ARITHMETIC_OVERFLOW = 7
OUT_OF_SPACE = 8
DESCRIPTOR_LIMIT = 9
POINTER_OUTSIDE = 10
MISALIGNED_POINTER = 11
INTERIOR_POINTER = 12
INVALID_POINTER = 13
DOUBLE_FREE = 14
CORRUPTED_STATE = 15
UINT64_MAX = (1 << 64) - 1
DESCRIPTORS = 512
MIN_ALIGNMENT = 16
MAX_ALIGNMENT = 4096


@dataclass
class Block:
    offset: int
    size: int
    live: bool
    requested: int = 0
    alignment: int = 0


class Oracle:
    def __init__(self, capacity: int) -> None:
        self.capacity = capacity
        self.committed = capacity
        self.blocks = [Block(0, capacity, False)]

    @staticmethod
    def alignment_valid(alignment: int) -> bool:
        return (
            alignment != 0
            and alignment <= MAX_ALIGNMENT
            and alignment & (alignment - 1) == 0
        )

    def allocate(self, size: int, alignment: int) -> tuple[int, int]:
        if size == 0:
            return ZERO_SIZE, UINT64_MAX
        if not self.alignment_valid(alignment):
            return INVALID_ALIGNMENT, UINT64_MAX

        effective = max(alignment, MIN_ALIGNMENT)
        descriptor_blocked = False
        for index, block in enumerate(self.blocks):
            if block.live:
                continue
            aligned = (block.offset + effective - 1) & ~(effective - 1)
            end = aligned + size
            block_end = block.offset + block.size
            if end > block_end:
                continue

            prefix = aligned - block.offset
            suffix = block_end - end
            added_descriptors = int(prefix != 0) + int(suffix != 0)
            if len(self.blocks) + added_descriptors > DESCRIPTORS:
                descriptor_blocked = True
                continue

            replacement: list[Block] = []
            if prefix:
                replacement.append(Block(block.offset, prefix, False))
            replacement.append(Block(aligned, size, True, size, effective))
            if suffix:
                replacement.append(Block(end, suffix, False))
            self.blocks[index : index + 1] = replacement
            return OK, aligned

        return (
            (DESCRIPTOR_LIMIT, UINT64_MAX)
            if descriptor_blocked
            else (OUT_OF_SPACE, UINT64_MAX)
        )

    def free(self, offset: int) -> int:
        if offset & (MIN_ALIGNMENT - 1):
            return MISALIGNED_POINTER
        if offset >= self.committed:
            return POINTER_OUTSIDE

        for index, block in enumerate(self.blocks):
            end = block.offset + block.size
            if offset == block.offset:
                if not block.live:
                    return DOUBLE_FREE
                block.live = False
                block.requested = 0
                block.alignment = 0
                if index > 0 and not self.blocks[index - 1].live:
                    self.blocks[index - 1].size += block.size
                    del self.blocks[index]
                    index -= 1
                    block = self.blocks[index]
                if index + 1 < len(self.blocks) and not self.blocks[index + 1].live:
                    block.size += self.blocks[index + 1].size
                    del self.blocks[index + 1]
                return OK
            if block.offset < offset < end:
                return INTERIOR_POINTER if block.live else DOUBLE_FREE
        return INVALID_POINTER

    def stats_line(self) -> str:
        live = [block for block in self.blocks if block.live]
        free = [block for block in self.blocks if not block.live]
        requested = sum(block.requested for block in live)
        live_extent = sum(block.size for block in live)
        reusable = sum(block.size for block in free)
        largest = max((block.size for block in free), default=0)
        return (
            f"S {OK} {self.capacity} {self.committed} {len(live)} "
            f"{requested} {live_extent} {reusable} {largest} "
            f"{len(self.blocks)} {DESCRIPTORS}"
        )


def append(
    commands: list[str], expected: list[str], command: str, result: str
) -> None:
    commands.append(command)
    expected.append(result)


def boundary_cases(commands: list[str], expected: list[str]) -> None:
    capacity = 32768
    append(commands, expected, f"I {capacity}", "I 0")
    append(commands, expected, f"G {capacity}", "G 0")
    append(commands, expected, "A 0 16", f"A {ZERO_SIZE} {UINT64_MAX}")
    append(
        commands,
        expected,
        "A 1 0",
        f"A {INVALID_ALIGNMENT} {UINT64_MAX}",
    )
    append(
        commands,
        expected,
        "A 1 3",
        f"A {INVALID_ALIGNMENT} {UINT64_MAX}",
    )
    append(
        commands,
        expected,
        "A 1 8192",
        f"A {INVALID_ALIGNMENT} {UINT64_MAX}",
    )
    append(commands, expected, f"A {capacity} 4096", "A 0 0")
    append(commands, expected, "A 1 1", f"A {OUT_OF_SPACE} {UINT64_MAX}")
    append(commands, expected, "F 1", f"F {MISALIGNED_POINTER}")
    append(commands, expected, "F 16", f"F {INTERIOR_POINTER}")
    append(commands, expected, "F 0", "F 0")
    append(commands, expected, "F 0", f"F {DOUBLE_FREE}")
    append(commands, expected, "N 18446744073709551615 4096",
        f"N {OUT_OF_SPACE} {UINT64_MAX}")
    append(commands, expected, "S",
        f"S 0 {capacity} {capacity} 0 0 0 {capacity} {capacity} 1 {DESCRIPTORS}")

    descriptor_capacity = 8192
    append(commands, expected, f"I {descriptor_capacity}", "I 0")
    append(commands, expected, f"G {descriptor_capacity}", "G 0")
    for index in range(DESCRIPTORS // 2):
        append(commands, expected, "A 1 1", f"A 0 {index * MIN_ALIGNMENT}")
    append(
        commands,
        expected,
        "A 1 1",
        f"A {DESCRIPTOR_LIMIT} {UINT64_MAX}",
    )
    append(
        commands,
        expected,
        "S",
        f"S 0 {descriptor_capacity} {descriptor_capacity} "
        f"{DESCRIPTORS // 2} {DESCRIPTORS // 2} {DESCRIPTORS // 2} "
        f"{descriptor_capacity - DESCRIPTORS // 2} "
        f"{descriptor_capacity - (DESCRIPTORS // 2 - 1) * MIN_ALIGNMENT - 1} "
        f"{DESCRIPTORS} {DESCRIPTORS}",
    )
    for index in range(DESCRIPTORS // 2):
        append(commands, expected, f"F {index * MIN_ALIGNMENT}", "F 0")
    append(
        commands,
        expected,
        "S",
        f"S 0 {descriptor_capacity} {descriptor_capacity} 0 0 0 "
        f"{descriptor_capacity} {descriptor_capacity} 1 {DESCRIPTORS}",
    )


def randomized_cases(
    commands: list[str], expected: list[str], count: int, seed: int
) -> None:
    capacity = 1024 * 1024
    oracle = Oracle(capacity)
    rng = random.Random(seed)
    alignments = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096]

    append(commands, expected, f"I {capacity}", "I 0")
    append(commands, expected, f"G {capacity}", "G 0")

    for case in range(count):
        live = [block for block in oracle.blocks if block.live]
        choice = rng.randrange(100)

        if choice < 58 or not live:
            size = rng.randrange(1, 8193)
            alignment = rng.choice(alignments)
            status, offset = oracle.allocate(size, alignment)
            append(
                commands,
                expected,
                f"A {size} {alignment}",
                f"A {status} {offset}",
            )
        elif choice < 92:
            block = rng.choice(live)
            status = oracle.free(block.offset)
            append(commands, expected, f"F {block.offset}", f"F {status}")
        elif choice < 94:
            append(commands, expected, "A 0 16", f"A {ZERO_SIZE} {UINT64_MAX}")
        elif choice < 96:
            append(
                commands,
                expected,
                "A 1 3",
                f"A {INVALID_ALIGNMENT} {UINT64_MAX}",
            )
        elif choice < 98:
            block = rng.choice(live)
            bad_offset = block.offset + 1
            append(
                commands,
                expected,
                f"F {bad_offset}",
                f"F {MISALIGNED_POINTER}",
            )
        else:
            append(
                commands,
                expected,
                f"F {capacity}",
                f"F {POINTER_OUTSIDE}",
            )

        if case % 97 == 0:
            append(commands, expected, "S", oracle.stats_line())

    append(commands, expected, "S", oracle.stats_line())
    append(commands, expected, f"I {capacity}", "I 0")
    append(commands, expected, f"G {capacity}", "G 0")
    append(commands, expected, "X 0", f"X {CORRUPTED_STATE}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases", type=int, default=100_000)
    parser.add_argument("--seed", type=int, default=0x5A454E49)
    arguments = parser.parse_args()

    if arguments.cases <= 0:
        parser.error("--cases must be positive")

    commands: list[str] = []
    expected: list[str] = []
    boundary_cases(commands, expected)
    randomized_cases(commands, expected, arguments.cases, arguments.seed)

    completed = subprocess.run(
        [arguments.runner],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
        timeout=180,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        print(f"heap-core runner exited {completed.returncode}", file=sys.stderr)
        return 1

    observed = completed.stdout.splitlines()
    if len(observed) != len(expected):
        print(
            f"line-count mismatch: observed {len(observed)}, expected {len(expected)}",
            file=sys.stderr,
        )
        return 1

    for index, (actual, wanted) in enumerate(zip(observed, expected, strict=True)):
        if actual != wanted:
            print(f"oracle mismatch at protocol line {index + 1}", file=sys.stderr)
            print(f" command:  {commands[index]}", file=sys.stderr)
            print(f" expected: {wanted}", file=sys.stderr)
            print(f" observed: {actual}", file=sys.stderr)
            return 1

    print(
        f"heap oracle passed {arguments.cases} deterministic randomized cases "
        f"(seed {arguments.seed})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

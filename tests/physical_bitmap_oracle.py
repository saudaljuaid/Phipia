#!/usr/bin/env python3
"""Compare the allocator's wordwise validation algebra to a bit oracle."""

from __future__ import annotations

import random


MASK = (1 << 64) - 1
RANDOM_CASES = 1_000_000
SEED = 0x5A17_0B1A


def word_population(value: int) -> int:
    value -= (value >> 1) & 0x5555555555555555
    value = (value & 0x3333333333333333) + (
        (value >> 2) & 0x3333333333333333
    )
    value = (value + (value >> 4)) & 0x0F0F0F0F0F0F0F0F
    value += value >> 8
    value += value >> 16
    value += value >> 32
    return value & 0x7F


def word_highest_set_bit(value: int) -> int:
    bit = 0
    if value >> 32:
        value >>= 32
        bit += 32
    if value >> 16:
        value >>= 16
        bit += 16
    if value >> 8:
        value >>= 8
        bit += 8
    if value >> 4:
        value >>= 4
        bit += 4
    if value >> 2:
        value >>= 2
        bit += 2
    if value >> 1:
        bit += 1
    return bit


def word_model(eligible: int, used: int, heap: int, task: int) -> tuple:
    owners = heap | task
    ineligible = (~eligible) & MASK
    allocated = eligible & used
    valid = not (
        (ineligible & ~used & MASK)
        or (owners & ineligible)
        or (owners & ~used & MASK)
        or (heap & task)
    )
    if not valid:
        return False, None, None

    allocatable_count = word_population(eligible)
    free_count = word_population(eligible & ~used & MASK)
    heap_count = word_population(heap)
    task_count = word_population(task)
    counts = (
        allocatable_count,
        64 - allocatable_count,
        allocatable_count - free_count,
        free_count,
        allocatable_count - free_count - heap_count - task_count,
        heap_count,
        task_count,
    )
    highest = word_highest_set_bit(eligible) if eligible else None
    return valid, counts, highest


def bit_model(eligible: int, used: int, heap: int, task: int) -> tuple:
    owners = heap | task
    allocated = eligible & used
    valid = not (
        (~eligible & ~used & MASK)
        or (owners & ~allocated & MASK)
        or (heap & task)
    )
    if not valid:
        return False, None, None

    counts = (
        eligible.bit_count(),
        ((~eligible) & MASK).bit_count(),
        allocated.bit_count(),
        (eligible & ~used & MASK).bit_count(),
        (allocated & ~owners & MASK).bit_count(),
        heap.bit_count(),
        task.bit_count(),
    )
    highest = eligible.bit_length() - 1 if eligible else None
    return valid, counts, highest


def main() -> None:
    rng = random.Random(SEED)
    fixed = [
        (0, 0, 0, 0),
        (0, MASK, 0, 0),
        (MASK, 0, 0, 0),
        (MASK, MASK, 0, 0),
        (MASK, MASK, MASK, 0),
        (MASK, MASK, 0, MASK),
        (MASK, MASK, MASK, MASK),
        (1, 1, 1, 0),
        (1 << 63, 1 << 63, 0, 1 << 63),
    ]

    for bit in range(64):
        bit_mask = 1 << bit
        for state in range(16):
            eligible = bit_mask if state & 1 else 0
            used = (MASK ^ bit_mask) | (bit_mask if state & 2 else 0)
            heap = bit_mask if state & 4 else 0
            task = bit_mask if state & 8 else 0
            fixed.append((eligible, used, heap, task))

    for case in fixed:
        assert word_model(*case) == bit_model(*case), case

    for index in range(RANDOM_CASES):
        eligible = rng.getrandbits(64)
        used = rng.getrandbits(64)

        if index % 2 == 0:
            used |= ~eligible & MASK
            allocated = eligible & used
            heap = allocated & rng.getrandbits(64)
            task = allocated & ~heap & rng.getrandbits(64)
            case = eligible, used, heap, task
        else:
            case = eligible, used, rng.getrandbits(64), rng.getrandbits(64)

        assert word_model(*case) == bit_model(*case), (index, case)

    print(
        "physical bitmap oracle: "
        f"{RANDOM_CASES:,} random tuples plus {len(fixed)} fixed cases passed "
        f"(seed={SEED})"
    )


if __name__ == "__main__":
    main()

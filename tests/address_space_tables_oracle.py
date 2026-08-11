#!/usr/bin/env python3
"""Independent randomized oracle for the bounded four-page user tables."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from dataclasses import dataclass


PAGE = 0x1000
ENTRIES = 512
PAGES = 4
LIMIT = 32
USER_BASE = 0x40000000
USER_END = 0x40200000
GUARD = 0x401FE000

PRESENT = 1 << 0
WRITABLE = 1 << 1
USER = 1 << 2
PWT = 1 << 3
PCD = 1 << 4
ACCESSED = 1 << 5
DIRTY = 1 << 6
HUGE = 1 << 7
NX = 1 << 63
ADDRESS_MASK = 0x000FFFFFFFFFF000
PARENT_ALLOWED = PRESENT | WRITABLE | PWT | PCD | ACCESSED | NX | ADDRESS_MASK
LEAF_ALLOWED = PRESENT | WRITABLE | USER | ACCESSED | DIRTY | NX | ADDRESS_MASK
PRIVATE_PARENT = PRESENT | WRITABLE | USER

READ = 1
WRITE = 2
EXECUTE = 4
USER_PERMISSION = 8
PERMISSION_MASK = 15

OK = 0
NULL_ARGUMENT = 1
NOT_CLEAR = 2
NOT_INITIALIZED = 3
BAD_ALIGNMENT = 4
BAD_PHYSICAL_ADDRESS = 5
FRAME_ALIAS = 6
INVALID_TEMPLATE = 7
NONCANONICAL_ADDRESS = 8
OUTSIDE_USER_APERTURE = 9
NULL_PAGE = 10
GUARD_PAGE = 11
INVALID_PERMISSIONS = 12
MAPPING_EXISTS = 13
MAPPING_NOT_FOUND = 14
MAPPING_LIMIT = 15
MAPPING_MISMATCH = 16
FRAME_IN_USE = 17
CORRUPTED = 18

PHYSICAL_MASK = 0x0000000FFFFFF000
KERNEL_PML4_PHYSICAL = 0x01000000
KERNEL_PDPT_PHYSICAL = 0x01001000
KERNEL_PD_PHYSICAL = 0x01002000
KERNEL_HIGH = (0x01003000, 0x01004000, 0x01005000)
PRIVATE_PHYSICAL = (0x02000000, 0x02001000, 0x02002000, 0x02003000)
USER_FRAME_BASE = 0x03000000
MASK64 = (1 << 64) - 1


def canonical(address: int) -> bool:
    address &= MASK64
    upper = address >> 48
    return upper == (0 if not address & (1 << 47) else 0xFFFF)


def physical_valid(address: int) -> bool:
    return address != 0 and address % PAGE == 0 and address & ~PHYSICAL_MASK == 0


def parent_valid(entry: int, allow_nx: bool) -> bool:
    address = entry & ADDRESS_MASK
    return (
        bool(entry & PRESENT)
        and not entry & USER
        and not entry & HUGE
        and not entry & ~PARENT_ALLOWED
        and (allow_nx or not entry & NX)
        and physical_valid(address)
    )


def address_status(address: int) -> int:
    address &= MASK64
    if not canonical(address):
        return NONCANONICAL_ADDRESS
    if address % PAGE:
        return BAD_ALIGNMENT
    if address < PAGE:
        return NULL_PAGE
    if address < USER_BASE or address >= USER_END:
        return OUTSIDE_USER_APERTURE
    if address == GUARD:
        return GUARD_PAGE
    return OK


def permissions_valid(permissions: int) -> bool:
    return (
        not permissions & ~PERMISSION_MASK
        and bool(permissions & READ)
        and bool(permissions & USER_PERMISSION)
        and not (permissions & WRITE and permissions & EXECUTE)
    )


def leaf(physical: int, permissions: int, retained: int = 0) -> int:
    value = physical | PRESENT | USER | (retained & (ACCESSED | DIRTY))
    if permissions & WRITE:
        value |= WRITABLE
    if not permissions & EXECUTE:
        value |= NX
    return value & MASK64


def leaf_permissions(value: int) -> int:
    permissions = READ | USER_PERMISSION
    if value & WRITABLE:
        permissions |= WRITE
    if not value & NX:
        permissions |= EXECUTE
    return permissions


@dataclass(frozen=True)
class Reply:
    status: int
    digest: int
    physical: int = 0
    permissions: int = 0
    accessed: int = 0
    dirty: int = 0


class Model:
    def __init__(self) -> None:
        self.table = [[0] * ENTRIES for _ in range(PAGES)]
        self.template_reset()
        self.clear()

    def template_reset(self) -> None:
        self.kernel_pml4 = [0] * ENTRIES
        self.kernel_pdpt = [0] * ENTRIES
        self.kernel_pml4[0] = KERNEL_PDPT_PHYSICAL | PRESENT | WRITABLE
        self.kernel_pml4[256] = KERNEL_HIGH[0] | PRESENT | WRITABLE
        self.kernel_pml4[288] = KERNEL_HIGH[1] | PRESENT | WRITABLE
        self.kernel_pml4[320] = KERNEL_HIGH[2] | PRESENT | WRITABLE
        self.kernel_pdpt[0] = KERNEL_PD_PHYSICAL | PRESENT | WRITABLE

    def clear(self) -> None:
        self.table = [[0] * ENTRIES for _ in range(PAGES)]
        self.initialized = 0
        self.mapping_count = 0
        self.mask = 0
        self.private = [0] * PAGES
        self.kernel_pml4_physical = 0
        self.kernel_pdpt_physical = 0

    def digest(self) -> int:
        digest = 1469598103934665603
        values = (
            [item for page in self.table for item in page]
            + [self.initialized, self.mapping_count, self.mask]
            + self.private
            + [self.kernel_pml4_physical, self.kernel_pdpt_physical]
        )
        for value in values:
            digest ^= value & MASK64
            digest = (digest * 1099511628211) & MASK64
        return digest

    def reply(
        self,
        status: int,
        physical: int = 0,
        permissions: int = 0,
        accessed: int = 0,
        dirty: int = 0,
    ) -> Reply:
        return Reply(status, self.digest(), physical, permissions, accessed, dirty)

    def template_valid(self) -> bool:
        if not parent_valid(self.kernel_pml4[0], False):
            return False
        if self.kernel_pml4[0] & ADDRESS_MASK != KERNEL_PDPT_PHYSICAL:
            return False
        if not parent_valid(self.kernel_pdpt[0], False):
            return False
        if self.kernel_pdpt[0] & ADDRESS_MASK in PRIVATE_PHYSICAL:
            return False
        if any(self.kernel_pml4[1:256]):
            return False
        for value in self.kernel_pml4[256:]:
            if value and (
                not parent_valid(value, True)
                or value & ADDRESS_MASK in PRIVATE_PHYSICAL
            ):
                return False
        return not any(self.kernel_pdpt[1:])

    def build(self) -> Reply:
        if (
            self.initialized
            or self.mapping_count
            or self.mask
            or any(self.private)
            or self.kernel_pml4_physical
            or self.kernel_pdpt_physical
            or any(any(page) for page in self.table)
        ):
            return self.reply(NOT_CLEAR)
        if not self.template_valid():
            return self.reply(INVALID_TEMPLATE)
        self.table[0][0] = PRIVATE_PHYSICAL[1] | PRIVATE_PARENT
        self.table[0][256:] = self.kernel_pml4[256:]
        self.table[1][0] = self.kernel_pdpt[0]
        self.table[1][1] = PRIVATE_PHYSICAL[2] | PRIVATE_PARENT
        self.table[2][0] = PRIVATE_PHYSICAL[3] | PRIVATE_PARENT
        self.initialized = 1
        self.mapping_count = 0
        self.mask = PHYSICAL_MASK
        self.private = list(PRIVATE_PHYSICAL)
        self.kernel_pml4_physical = KERNEL_PML4_PHYSICAL
        self.kernel_pdpt_physical = KERNEL_PDPT_PHYSICAL
        return self.reply(OK)

    def validate_status(self) -> int:
        if not self.initialized:
            return NOT_INITIALIZED
        if self.initialized != 1 or self.mapping_count > LIMIT:
            return CORRUPTED
        if (
            self.mask != PHYSICAL_MASK
            or tuple(self.private) != PRIVATE_PHYSICAL
            or self.kernel_pml4_physical != KERNEL_PML4_PHYSICAL
            or self.kernel_pdpt_physical != KERNEL_PDPT_PHYSICAL
            or not self.template_valid()
        ):
            return CORRUPTED
        if (self.table[0][0] & ~ACCESSED) != (PRIVATE_PHYSICAL[1] | PRIVATE_PARENT):
            return CORRUPTED
        if (self.table[1][0] & ~ACCESSED) != (self.kernel_pdpt[0] & ~ACCESSED):
            return CORRUPTED
        if (self.table[1][1] & ~ACCESSED) != (PRIVATE_PHYSICAL[2] | PRIVATE_PARENT):
            return CORRUPTED
        if (self.table[2][0] & ~ACCESSED) != (PRIVATE_PHYSICAL[3] | PRIVATE_PARENT):
            return CORRUPTED
        if any(self.table[0][1:256]):
            return CORRUPTED
        for observed, expected in zip(self.table[0][256:], self.kernel_pml4[256:]):
            if observed & ~ACCESSED != expected & ~ACCESSED:
                return CORRUPTED
        if any(self.table[1][2:]) or any(self.table[2][1:]):
            return CORRUPTED
        seen: set[int] = set()
        mapped = 0
        guard_index = (GUARD - USER_BASE) // PAGE
        for index, value in enumerate(self.table[3]):
            if not value:
                continue
            physical = value & ADDRESS_MASK
            if (
                not value & PRESENT
                or not value & USER
                or value & ~LEAF_ALLOWED
                or (value & WRITABLE and not value & NX)
                or index == guard_index
                or not physical_valid(physical)
                or physical in PRIVATE_PHYSICAL
                or physical in seen
            ):
                return CORRUPTED
            seen.add(physical)
            mapped += 1
            if mapped > LIMIT:
                return CORRUPTED
        return OK if mapped == self.mapping_count else CORRUPTED

    def validate(self) -> Reply:
        return self.reply(self.validate_status())

    def map(self, virtual: int, physical: int, permissions: int) -> Reply:
        status = self.validate_status()
        if status != OK:
            return self.reply(status)
        status = address_status(virtual)
        if status != OK:
            return self.reply(status)
        if not permissions_valid(permissions):
            return self.reply(INVALID_PERMISSIONS)
        if not physical_valid(physical):
            return self.reply(BAD_PHYSICAL_ADDRESS)
        if physical in PRIVATE_PHYSICAL:
            return self.reply(FRAME_ALIAS)
        index = (virtual - USER_BASE) // PAGE
        if self.table[3][index]:
            return self.reply(MAPPING_EXISTS)
        if any(value and value & ADDRESS_MASK == physical for value in self.table[3]):
            return self.reply(FRAME_IN_USE)
        if self.mapping_count == LIMIT:
            return self.reply(MAPPING_LIMIT)
        self.table[3][index] = leaf(physical, permissions)
        self.mapping_count += 1
        return self.reply(OK)

    def unmap(self, virtual: int, expected: int) -> Reply:
        status = self.validate_status()
        if status != OK:
            return self.reply(status)
        status = address_status(virtual)
        if status != OK:
            return self.reply(status)
        if not physical_valid(expected):
            return self.reply(BAD_PHYSICAL_ADDRESS)
        index = (virtual - USER_BASE) // PAGE
        value = self.table[3][index]
        if not value:
            return self.reply(MAPPING_NOT_FOUND)
        if value & ADDRESS_MASK != expected:
            return self.reply(MAPPING_MISMATCH)
        self.table[3][index] = 0
        self.mapping_count -= 1
        return self.reply(OK)

    def repermission(self, virtual: int, permissions: int) -> Reply:
        status = self.validate_status()
        if status != OK:
            return self.reply(status)
        status = address_status(virtual)
        if status != OK:
            return self.reply(status)
        if not permissions_valid(permissions):
            return self.reply(INVALID_PERMISSIONS)
        index = (virtual - USER_BASE) // PAGE
        value = self.table[3][index]
        if not value:
            return self.reply(MAPPING_NOT_FOUND)
        self.table[3][index] = leaf(value & ADDRESS_MASK, permissions, value)
        return self.reply(OK)

    def query(self, virtual: int) -> Reply:
        status = self.validate_status()
        if status != OK:
            return self.reply(status)
        status = address_status(virtual)
        if status != OK:
            return self.reply(status)
        value = self.table[3][(virtual - USER_BASE) // PAGE]
        if not value:
            return self.reply(MAPPING_NOT_FOUND)
        return self.reply(
            OK,
            value & ADDRESS_MASK,
            leaf_permissions(value),
            int(bool(value & ACCESSED)),
            int(bool(value & DIRTY)),
        )


def random_virtual(rng: random.Random) -> int:
    choice = rng.randrange(10)
    if choice < 6:
        return USER_BASE + rng.randrange(ENTRIES) * PAGE
    if choice == 6:
        return rng.choice((0, GUARD, USER_END, USER_BASE - PAGE))
    if choice == 7:
        return USER_BASE + rng.randrange(ENTRIES) * PAGE + rng.randrange(1, PAGE)
    if choice == 8:
        return 0x0000800000000000
    return 0xFFFF800000000000


def random_physical(model: Model, rng: random.Random) -> int:
    choice = rng.randrange(10)
    present = [value & ADDRESS_MASK for value in model.table[3] if value]
    if choice < 6:
        return USER_FRAME_BASE + rng.randrange(4096) * PAGE
    if choice == 6 and present:
        return rng.choice(present)
    if choice == 7:
        return rng.choice(PRIVATE_PHYSICAL)
    if choice == 8:
        return rng.choice((0, USER_FRAME_BASE + 1))
    return PHYSICAL_MASK + PAGE


def random_permissions(rng: random.Random) -> int:
    if rng.randrange(4):
        return rng.choice((READ | USER_PERMISSION, READ | WRITE | USER_PERMISSION,
                           READ | EXECUTE | USER_PERMISSION))
    return rng.randrange(32)


def generate_cases(count: int, seed: int) -> tuple[str, list[Reply]]:
    rng = random.Random(seed)
    model = Model()
    commands: list[str] = []
    expected: list[Reply] = []

    def add(command: str, reply: Reply) -> None:
        commands.append(command)
        expected.append(reply)

    add("build", model.build())
    while len(commands) < count:
        roll = rng.randrange(100)
        if roll < 4:
            model.clear()
            add("clear", model.reply(OK))
        elif roll < 8:
            model.template_reset()
            add("template-reset", model.reply(OK))
        elif roll < 14:
            add("build", model.build())
        elif roll < 43:
            virtual = random_virtual(rng)
            physical = random_physical(model, rng)
            permissions = random_permissions(rng)
            add(
                f"map {virtual:#x} {physical:#x} {permissions:#x}",
                model.map(virtual, physical, permissions),
            )
        elif roll < 58:
            virtual = random_virtual(rng)
            physical = random_physical(model, rng)
            add(f"unmap {virtual:#x} {physical:#x}", model.unmap(virtual, physical))
        elif roll < 70:
            virtual = random_virtual(rng)
            permissions = random_permissions(rng)
            add(
                f"repermission {virtual:#x} {permissions:#x}",
                model.repermission(virtual, permissions),
            )
        elif roll < 84:
            virtual = random_virtual(rng)
            add(f"query {virtual:#x}", model.query(virtual))
        elif roll < 94:
            add("validate", model.validate())
        elif roll < 98:
            page = rng.randrange(PAGES)
            index = rng.randrange(ENTRIES)
            value = rng.getrandbits(64)
            model.table[page][index] = value
            add(f"table-set {page} {index} {value:#x}", model.reply(OK))
        else:
            page = rng.randrange(2)
            index = rng.randrange(ENTRIES)
            value = rng.getrandbits(64)
            if page == 0:
                model.kernel_pml4[index] = value
            else:
                model.kernel_pdpt[index] = value
            add(f"template-set {page} {index} {value:#x}", model.reply(OK))

    commands.append("quit")
    return "\n".join(commands) + "\n", expected


def parse_reply(line: str) -> Reply:
    fields = line.split()
    if len(fields) != 6:
        raise ValueError(f"malformed runner reply: {line!r}")
    return Reply(
        int(fields[0]),
        int(fields[1], 16),
        int(fields[2], 16),
        int(fields[3], 16),
        int(fields[4]),
        int(fields[5]),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("--cases", type=int, default=250_000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x41535436)
    args = parser.parse_args()
    if args.cases < 250_000:
        parser.error("--cases must be at least 250000")

    protocol, expected = generate_cases(args.cases, args.seed)
    completed = subprocess.run(
        [args.runner, "protocol"],
        input=protocol,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr)
        return completed.returncode or 1
    observed_lines = completed.stdout.splitlines()
    if len(observed_lines) != len(expected):
        print(
            f"reply count mismatch: observed={len(observed_lines)} expected={len(expected)}",
            file=sys.stderr,
        )
        return 1
    for index, (line, wanted) in enumerate(zip(observed_lines, expected)):
        try:
            observed = parse_reply(line)
        except ValueError as error:
            print(f"case {index}: {error}", file=sys.stderr)
            return 1
        if observed != wanted:
            print(
                f"case {index} mismatch:\n"
                f"  command={protocol.splitlines()[index]!r}\n"
                f"  observed={observed}\n"
                f"  expected={wanted}",
                file=sys.stderr,
            )
            return 1
    print(
        f"address-space table oracle passed: cases={args.cases} seed={args.seed:#x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Say where the image's pages actually go.

`elf-audit.py` reports one number: the footprint in pages. That number has been
recorded in the platform contract after every change, and reading it as "the
program has grown" turned out to be wrong in a way that mattered — sixteen of
the forty-two pages are a single constant, `sapstudio_rt::HEAP`, which is a
reservation rather than anything the program contains.

So this splits the number. It reads the loaded sections and the symbol table,
attributes every sized symbol to the crate that emitted it, and prints the
result largest first. What it produces is evidence for a decision that has to
be made eventually — whether to split the program so the freestanding image
links less of it — and evidence is what that decision has been missing.

It refuses nothing. A budget that failed a build would be a guess at what the
right size is, and nobody knows that until `SAP-03` says what a program is
given. This measures; the platform contract judges.
"""

from __future__ import annotations

import signal
import struct
import sys
from pathlib import Path

# This prints a report a person reads, so somebody will pipe it into `head` or
# `less` and close the pipe early. Without this that is a traceback rather than
# a clean stop, and a tool that appears to crash when it is read is a tool
# people stop reading.
if hasattr(signal, "SIGPIPE"):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

PAGE = 4096

#: Sections that occupy memory when the program runs.
LOADED = (".text", ".rodata", ".data", ".bss")


def sections(raw: bytes) -> list[tuple[str, int, int, int, int]]:
    """Every section: name, kind, address, file offset, size."""
    (shoff,) = struct.unpack_from("<Q", raw, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 0x3A)
    base = shoff + shstrndx * shentsize
    strtab_offset, strtab_size = struct.unpack_from("<QQ", raw, base + 24)
    names = raw[strtab_offset : strtab_offset + strtab_size]

    found = []
    for index in range(shnum):
        header = shoff + index * shentsize
        (name_offset, kind) = struct.unpack_from("<II", raw, header)
        address, offset, size = struct.unpack_from("<QQQ", raw, header + 16)
        end = names.index(b"\0", name_offset)
        found.append((names[name_offset:end].decode(), kind, address, offset, size))
    return found


def symbols(raw: bytes, table: list) -> list[tuple[int, str, str]]:
    """Every sized symbol: size, the section it is in, and its name."""
    by_name = {entry[0]: entry for entry in table}
    if ".symtab" not in by_name or ".strtab" not in by_name:
        return []
    _, _, _, sym_offset, sym_size = by_name[".symtab"]
    _, _, _, str_offset, str_size = by_name[".strtab"]
    strings = raw[str_offset : str_offset + str_size]

    found = []
    for base in range(sym_offset, sym_offset + sym_size, 24):
        (name_offset,) = struct.unpack_from("<I", raw, base)
        (section_index,) = struct.unpack_from("<H", raw, base + 6)
        (size,) = struct.unpack_from("<Q", raw, base + 16)
        if name_offset == 0 or size == 0:
            continue
        end = strings.index(b"\0", name_offset)
        name = strings[name_offset:end].decode()
        where = table[section_index][0] if section_index < len(table) else "?"
        found.append((size, where, name))
    return found


def crate_of(symbol: str) -> str:
    """Which crate emitted a symbol, from its mangled name.

    Rust's legacy mangling writes `_ZN` then length-prefixed path components,
    so the first component is the crate. A generic instantiated in one crate
    from another's code is attributed to whichever crate's name comes first,
    which is a rough edge and an honest one: the alternative is claiming a
    precision the mangling does not carry.
    """
    if not symbol.startswith("_ZN"):
        return "(unmangled)"
    rest = symbol[3:]
    digits = ""
    while rest and rest[0].isdigit():
        digits += rest[0]
        rest = rest[1:]
    if not digits:
        return "(unmangled)"
    first = rest[: int(digits)]
    return first if first.startswith("sapstudio") else f"(rust: {first})"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: footprint.py <image>", file=sys.stderr)
        return 2
    raw = Path(sys.argv[1]).read_bytes()
    table = sections(raw)
    sizes = {name: size for name, _, _, _, size in table}

    total = sum(sizes.get(name, 0) for name in LOADED)
    print(f"footprint  {total} bytes, {(total + PAGE - 1) // PAGE} pages\n")

    print("by section")
    for name in LOADED:
        size = sizes.get(name, 0)
        if size:
            share = 100 * size / total
            print(f"  {name:9s} {size:8d}  {size / PAGE:5.1f} pages  {share:4.1f}%")

    held = symbols(raw, table)
    if not held:
        print("\nno symbol table: build with debug information to see the rest")
        return 0

    print("\nlargest single symbols")
    for size, where, name in sorted(held, reverse=True)[:10]:
        print(f"  {size:8d}  {where:8s} {name[:64]}")

    per_crate: dict[str, int] = {}
    for size, _, name in held:
        crate = crate_of(name)
        per_crate[crate] = per_crate.get(crate, 0) + size
    attributed = sum(per_crate.values())

    print("\nby crate, from the symbols that carry a size")
    for crate, size in sorted(per_crate.items(), key=lambda pair: -pair[1]):
        print(f"  {size:8d}  {size / PAGE:5.1f} pages  {crate}")
    print(f"  {attributed:8d}  {attributed / PAGE:5.1f} pages  attributed in total")
    print(
        f"  {total - attributed:8d}  {(total - attributed) / PAGE:5.1f} pages  "
        "not attributed: padding, literals, and anything the table does not size"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

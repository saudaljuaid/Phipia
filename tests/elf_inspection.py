#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the kernel ELF and its Multiboot2 header without third-party code."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
MULTIBOOT2_MAGIC = 0xE85250D6
PT_LOAD = 1
PF_X = 1
PF_W = 2
SHF_WRITE = 1
SHF_ALLOC = 2
SHF_EXECINSTR = 4
SHT_NOBITS = 8


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def c_string(table: bytes, offset: int) -> str:
    require(offset < len(table), "section name offset is out of range")
    end = table.find(b"\0", offset)
    require(end >= 0, "section name is not terminated")
    return table[offset:end].decode("ascii", errors="strict")


def inspect(path: Path) -> None:
    image = path.read_bytes()
    require(len(image) >= ELF_HEADER.size, "ELF header is truncated")
    fields = ELF_HEADER.unpack_from(image)
    ident = fields[0]
    require(ident[:4] == b"\x7fELF", "ELF magic is invalid")
    require(ident[4] == 2, "kernel is not ELF64")
    require(ident[5] == 1, "kernel is not little-endian")
    require(fields[1] == 2, "kernel ELF type is not executable")
    require(fields[2] == 62, "kernel ELF machine is not AMD64")

    phoff, shoff = fields[5], fields[6]
    phentsize, phnum = fields[9], fields[10]
    shentsize, shnum, shstrndx = fields[11], fields[12], fields[13]
    require(phentsize == PROGRAM_HEADER.size, "program-header size is invalid")
    require(shentsize == SECTION_HEADER.size, "section-header size is invalid")
    require(phoff + phnum * phentsize <= len(image), "program headers overflow")
    require(shoff + shnum * shentsize <= len(image), "section headers overflow")
    require(shstrndx < shnum, "section-name table index is invalid")

    programs = [
        PROGRAM_HEADER.unpack_from(image, phoff + index * phentsize)
        for index in range(phnum)
    ]
    load_segments = []
    for index, program in enumerate(programs):
        kind, flags, offset, virtual, _physical, file_size, memory_size, align = (
            program
        )
        require(file_size <= memory_size, f"segment {index} filesz exceeds memsz")
        require(offset + file_size <= len(image), f"segment {index} overflows file")
        if align > 1:
            require(align & (align - 1) == 0, f"segment {index} bad alignment")
            require(
                virtual % align == offset % align,
                f"segment {index} virtual/file alignment mismatch",
            )
        if kind == PT_LOAD:
            require(
                flags & (PF_W | PF_X) != (PF_W | PF_X),
                f"load segment {index} is writable and executable",
            )
            load_segments.append(program)

    require(load_segments, "kernel has no load segment")

    sections = [
        SECTION_HEADER.unpack_from(image, shoff + index * shentsize)
        for index in range(shnum)
    ]
    names_header = sections[shstrndx]
    names_offset, names_size = names_header[4], names_header[5]
    require(names_offset + names_size <= len(image), "section names overflow file")
    names = image[names_offset : names_offset + names_size]
    named_sections: dict[str, tuple[int, ...]] = {}

    for index, section in enumerate(sections):
        name = c_string(names, section[0])
        named_sections[name] = section
        kind, flags, address, offset, size = (
            section[1],
            section[2],
            section[3],
            section[4],
            section[5],
        )
        if kind != SHT_NOBITS:
            require(offset + size <= len(image), f"section {name!r} overflows file")
        if flags & SHF_ALLOC:
            covering = [
                program
                for program in load_segments
                if address >= program[3]
                and address + size <= program[3] + program[6]
            ]
            require(covering, f"allocated section {name!r} is outside LOAD segments")
            if flags & SHF_EXECINSTR:
                require(not flags & SHF_WRITE, f"section {name!r} is W+X")

    expected = {
        ".multiboot": SHF_ALLOC,
        ".text": SHF_ALLOC | SHF_EXECINSTR,
        ".rodata": SHF_ALLOC,
        ".data": SHF_ALLOC | SHF_WRITE,
        ".bss": SHF_ALLOC | SHF_WRITE,
    }
    for name, wanted_flags in expected.items():
        require(name in named_sections, f"required section {name!r} is missing")
        observed = named_sections[name][2] & (
            SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR
        )
        require(observed == wanted_flags, f"section {name!r} permissions differ")

    magic = struct.pack("<I", MULTIBOOT2_MAGIC)
    candidates = [
        offset
        for offset in range(0, min(len(image), 32768), 8)
        if image[offset : offset + 4] == magic
    ]
    require(len(candidates) == 1, "expected exactly one aligned Multiboot2 header")
    header_offset = candidates[0]
    magic_value, architecture, length, checksum = struct.unpack_from(
        "<IIII", image, header_offset
    )
    require(magic_value == MULTIBOOT2_MAGIC, "Multiboot2 magic changed")
    require(architecture == 0, "Multiboot2 architecture is not i386-compatible")
    require(length >= 24 and length % 8 == 0, "Multiboot2 length is invalid")
    require(header_offset + length <= 32768, "Multiboot2 header is too late")
    require(
        (magic_value + architecture + length + checksum) & 0xFFFFFFFF == 0,
        "Multiboot2 checksum is invalid",
    )
    cursor = header_offset + 16
    end = header_offset + length
    end_tags = 0
    while cursor < end:
        require(cursor + 8 <= end, "Multiboot2 tag header is truncated")
        tag_type, flags, size = struct.unpack_from("<HHI", image, cursor)
        require(size >= 8, "Multiboot2 tag size is invalid")
        require(cursor + size <= end, "Multiboot2 tag overflows header")
        if tag_type == 0:
            require(flags == 0 and size == 8, "Multiboot2 end tag is malformed")
            end_tags += 1
            require(cursor + 8 == end, "Multiboot2 end tag is not last")
        cursor += (size + 7) & ~7
    require(cursor == end and end_tags == 1, "Multiboot2 tag walk is invalid")

    print(
        f"ELF inspection passed: {phnum} program headers, {shnum} sections, "
        f"{len(load_segments)} LOAD segments, Multiboot2 offset {header_offset}"
    )
    for index, program in enumerate(load_segments):
        flags = "".join(
            letter if program[1] & bit else "-"
            for letter, bit in (("R", 4), ("W", 2), ("X", 1))
        )
        print(
            f"  LOAD {index}: {flags} vaddr=0x{program[3]:016x} "
            f"filesz={program[5]} memsz={program[6]}"
        )
    for name in expected:
        section = named_sections[name]
        print(f"  {name}: address=0x{section[3]:016x} size={section[5]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    arguments = parser.parse_args()
    try:
        inspect(arguments.elf)
    except (OSError, ValueError, struct.error, UnicodeDecodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

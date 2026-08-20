#!/usr/bin/env python3
"""Pack printable ASCII from Spleen 8x16 BDF into Pyrenis UI Font v1."""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path


MAGIC = b"PUF1"
VERSION = 1
HEADER_LENGTH = 24
WIDTH = 8
HEIGHT = 16
ASCENT = 12
DESCENT = 4
ADVANCE = 8
ROW_BYTES = 1
FIRST = 0x20
COUNT = 0x7F - FIRST


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"UI font source refusal: {message}")


def parse_bdf(path: Path) -> dict[int, bytes]:
    lines = path.read_text(encoding="ascii").splitlines()
    required = {
        f"FONTBOUNDINGBOX {WIDTH} {HEIGHT} 0 -4",
        f"FONT_ASCENT {ASCENT}",
        f"FONT_DESCENT {DESCENT}",
    }
    if not required.issubset(set(lines)):
        fail("Spleen metrics do not match the pinned 8x16 contract")

    glyphs: dict[int, bytes] = {}
    index = 0
    while index < len(lines):
        if not lines[index].startswith("STARTCHAR "):
            index += 1
            continue

        encoding: int | None = None
        dwidth: tuple[int, int] | None = None
        box: tuple[int, int, int, int] | None = None
        bitmap: list[str] = []
        index += 1

        while index < len(lines) and lines[index] != "ENDCHAR":
            line = lines[index]
            if line.startswith("ENCODING "):
                encoding = int(line.split()[1], 10)
            elif line.startswith("DWIDTH "):
                parts = line.split()
                dwidth = (int(parts[1], 10), int(parts[2], 10))
            elif line.startswith("BBX "):
                parts = line.split()
                box = tuple(int(value, 10) for value in parts[1:5])
            elif line == "BITMAP":
                bitmap = lines[index + 1:index + 1 + HEIGHT]
                index += HEIGHT
            index += 1

        if index >= len(lines):
            fail("unterminated BDF glyph")

        if encoding is not None and FIRST <= encoding < FIRST + COUNT:
            if encoding in glyphs:
                fail(f"duplicate glyph U+{encoding:04X}")
            if dwidth != (ADVANCE, 0):
                fail(f"bad advance for U+{encoding:04X}")
            if box != (WIDTH, HEIGHT, 0, -DESCENT):
                fail(f"bad bitmap box for U+{encoding:04X}")
            if len(bitmap) != HEIGHT:
                fail(f"truncated bitmap for U+{encoding:04X}")

            packed = bytearray()
            for row in bitmap:
                if len(row) != ROW_BYTES * 2:
                    fail(f"bad row width for U+{encoding:04X}")
                try:
                    value = int(row, 16)
                except ValueError:
                    fail(f"non-hex row for U+{encoding:04X}")
                if value >= (1 << (ROW_BYTES * 8)):
                    fail(f"glyph U+{encoding:04X} uses pixels past width {WIDTH}")
                packed.extend(value.to_bytes(ROW_BYTES, "big"))
            glyphs[encoding] = bytes(packed)

        index += 1

    missing = [code for code in range(FIRST, FIRST + COUNT) if code not in glyphs]
    if missing:
        fail("missing printable ASCII glyph " + f"U+{missing[0]:04X}")
    return glyphs


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: make-ui-font-asset.py INPUT.bdf OUTPUT.puf")

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    glyphs = parse_bdf(source)
    data = b"".join(glyphs[code] for code in range(FIRST, FIRST + COUNT))
    header = struct.pack(
        "<4s8BIII",
        MAGIC,
        VERSION,
        HEADER_LENGTH,
        WIDTH,
        HEIGHT,
        ASCENT,
        DESCENT,
        ADVANCE,
        ROW_BYTES,
        FIRST,
        COUNT,
        len(data),
    )
    blob = header + data
    output.write_bytes(blob)
    digest = hashlib.sha256(blob).hexdigest().upper()
    print(
        f"{source}: {COUNT} glyphs U+{FIRST:04X}-U+{FIRST + COUNT - 1:04X}, "
        f"{WIDTH}x{HEIGHT}, {len(blob)} bytes, SHA-256 {digest} -> {output}"
    )


if __name__ == "__main__":
    main()

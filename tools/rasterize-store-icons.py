#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Developer tool: build the committed monochrome Store Lucide sprite."""

from __future__ import annotations

import io
import sys
from pathlib import Path

import cairosvg
from PIL import Image


NAMES = (
    "house", "package-check", "refresh-cw", "settings",
    "info", "layout-grid", "accessibility", "code-xml",
    "gamepad-2", "image", "network", "music-2",
    "flask-conical", "monitor-cog", "wrench", "package-open",
)
TILE = 32
GLYPH = 20
COLUMNS = 8
SCALE = 4


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: rasterize-store-icons.py LUCIDE_DIR OUTPUT.png"
        )
    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    rows = (len(NAMES) + COLUMNS - 1) // COLUMNS
    sheet = Image.new(
        "RGBA", (TILE * COLUMNS * SCALE, TILE * rows * SCALE),
        (0, 0, 0, 0),
    )
    inset = (TILE - GLYPH) // 2

    for index, name in enumerate(NAMES):
        svg = (source / f"{name}.svg").read_text(encoding="utf-8")
        svg = svg.replace("currentColor", "#20252A")
        glyph_bytes = cairosvg.svg2png(
            bytestring=svg.encode("utf-8"),
            output_width=GLYPH * SCALE,
            output_height=GLYPH * SCALE,
        )
        glyph = Image.open(io.BytesIO(glyph_bytes)).convert("RGBA")
        x = ((index % COLUMNS) * TILE + inset) * SCALE
        y = ((index // COLUMNS) * TILE + inset) * SCALE
        sheet.alpha_composite(glyph, (x, y))

    sheet = sheet.resize(
        (TILE * COLUMNS, TILE * rows), Image.Resampling.LANCZOS
    )
    sheet.save(destination, format="PNG", optimize=True)


if __name__ == "__main__":
    main()

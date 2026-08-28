#!/usr/bin/env python3
"""Developer tool: build the committed Settings icon sprite from Lucide SVGs."""

from __future__ import annotations

import io
import sys
from pathlib import Path

import cairosvg
from PIL import Image, ImageDraw


NAMES = (
    "palette", "image", "panel-bottom", "monitor",
    "keyboard", "mouse-pointer-2", "volume-2", "network",
    "hard-drive", "clock-3", "rocket", "info",
)
COLOURS = (
    (58, 158, 202), (52, 126, 199), (87, 93, 104), (84, 123, 190),
    (111, 119, 126), (119, 128, 133), (137, 91, 189), (49, 157, 111),
    (217, 153, 48), (204, 131, 47), (101, 118, 144), (216, 74, 69),
)
TILE = 64
SCALE = 4


def vertical_gradient(image: Image.Image, box: tuple[int, int, int, int],
                      colour: tuple[int, int, int]) -> None:
    left, top, right, bottom = box
    for y in range(top, bottom):
        ratio = (y - top) / max(1, bottom - top - 1)
        highlight = 1.24 - ratio * 0.42
        pixel = tuple(max(0, min(255, round(channel * highlight)))
                      for channel in colour) + (255,)
        ImageDraw.Draw(image).line((left, y, right - 1, y), fill=pixel)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: rasterize-settings-icons.py LUCIDE_DIR OUTPUT.png")
    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    sheet = Image.new("RGBA", (TILE * 4 * SCALE, TILE * 3 * SCALE),
                      (0, 0, 0, 0))
    for index, (name, colour) in enumerate(zip(NAMES, COLOURS)):
        tile = Image.new("RGBA", (TILE * SCALE, TILE * SCALE), (0, 0, 0, 0))
        plate = Image.new("RGBA", tile.size, (0, 0, 0, 0))
        plate_box = (7 * SCALE, 5 * SCALE, 57 * SCALE, 55 * SCALE)
        vertical_gradient(plate, plate_box, colour)
        alpha = Image.new("L", tile.size, 0)
        ImageDraw.Draw(alpha).rounded_rectangle(
            plate_box, radius=13 * SCALE, fill=255,
            outline=(255,), width=SCALE,
        )
        plate.putalpha(alpha)
        tile.alpha_composite(plate)
        gloss = Image.new("RGBA", tile.size, (0, 0, 0, 0))
        ImageDraw.Draw(gloss).rounded_rectangle(
            (9 * SCALE, 7 * SCALE, 55 * SCALE, 28 * SCALE),
            radius=10 * SCALE, fill=(255, 255, 255, 52),
        )
        tile.alpha_composite(gloss)
        svg = (source / f"{name}.svg").read_text(encoding="utf-8")
        svg = svg.replace("currentColor", "#FFFFFF")
        glyph_bytes = cairosvg.svg2png(
            bytestring=svg.encode("utf-8"),
            output_width=30 * SCALE,
            output_height=30 * SCALE,
        )
        glyph = Image.open(io.BytesIO(glyph_bytes)).convert("RGBA")
        tile.alpha_composite(glyph, (17 * SCALE, 15 * SCALE))
        outline = ImageDraw.Draw(tile)
        outline.rounded_rectangle(plate_box, radius=13 * SCALE,
                                  outline=(35, 42, 47, 150), width=SCALE)
        x = (index % 4) * TILE * SCALE
        y = (index // 4) * TILE * SCALE
        sheet.alpha_composite(tile, (x, y))
    sheet = sheet.resize((TILE * 4, TILE * 3), Image.Resampling.LANCZOS)
    sheet.save(destination, format="PNG", optimize=True)


if __name__ == "__main__":
    main()

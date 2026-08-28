#!/usr/bin/env python3
"""Developer tool: prepare supplied app artwork for a transparent dock."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw


SIZE = 256
INSET = 5
RADIUS = 30


def prepare(source: Path, destination: Path) -> None:
    image = Image.open(source).convert("RGBA")
    side = min(image.size)
    left = (image.width - side) // 2
    top = (image.height - side) // 2
    image = image.crop((left, top, left + side, top + side))
    image = image.resize((SIZE - INSET * 2, SIZE - INSET * 2),
                         Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.alpha_composite(image, (INSET, INSET))
    scale = 4
    mask = Image.new("L", (SIZE * scale, SIZE * scale), 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle(
        (INSET * scale, INSET * scale,
         (SIZE - INSET - 1) * scale, (SIZE - INSET - 1) * scale),
        radius=RADIUS * scale,
        fill=255,
    )
    mask = mask.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    canvas.putalpha(mask)
    canvas.save(destination, format="PNG", optimize=True)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-dock-icons.py SOURCE.png OUTPUT.png")
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the deterministic, bounded Sapote desktop wallpaper asset.

The source remains a normal PNG.  This tool performs all PNG and scaling work
on the host, then emits SPW1: geometry, a fixed RGB332 palette, and one checked
palette index per output pixel.  The kernel therefore needs no PNG or DEFLATE
implementation and the committed photograph can be reproduced byte-for-byte.
"""

import struct
import sys
import zlib


MAGIC = b"SPW1"
OUT_WIDTH = 1024
OUT_HEIGHT = 768
BAYER = ((0, 8, 2, 10), (12, 4, 14, 6),
         (3, 11, 1, 9), (15, 7, 13, 5))


def unfilter(raw, width, height, channels):
    stride = width * channels
    output = bytearray()
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += stride + 1
        for index in range(stride):
            left = line[index - channels] if index >= channels else 0
            up = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - upper_left
                distances = (abs(estimate - left), abs(estimate - up),
                             abs(estimate - upper_left))
                nearest = (left, up, upper_left)[distances.index(min(distances))]
                line[index] = (line[index] + nearest) & 0xFF
            elif kind != 0:
                raise SystemExit("unsupported PNG filter")
        output += line
        previous = line
    return output


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("wallpaper source is not a PNG")
    offset = 8
    header = None
    compressed = bytearray()
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body[:13])
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        offset += length + 12
    if header is None:
        raise SystemExit("wallpaper PNG has no IHDR")
    width, height, depth, colour, compression, filtering, interlace = header
    if depth != 8 or colour not in (2, 6) or compression != 0 or filtering != 0 or interlace != 0:
        raise SystemExit("expected a non-interlaced 8-bit RGB or RGBA PNG")
    channels = 3 if colour == 2 else 4
    return width, height, channels, unfilter(
        zlib.decompress(bytes(compressed)), width, height, channels)


def expand(bits, value):
    maximum = (1 << bits) - 1
    return (value * 255 + maximum // 2) // maximum


def palette():
    result = bytearray()
    for index in range(256):
        result += bytes((expand(3, index >> 5),
                         expand(3, (index >> 2) & 7),
                         expand(2, index & 3)))
    return result


def quantize(channel, levels, threshold):
    adjusted = max(0, min(255, channel + threshold))
    return (adjusted * (levels - 1) + 127) // 255


def build_indices(source, width, height, channels):
    result = bytearray(OUT_WIDTH * OUT_HEIGHT)
    for y in range(OUT_HEIGHT):
        source_y = min(height - 1, (y * height + OUT_HEIGHT // 2) // OUT_HEIGHT)
        for x in range(OUT_WIDTH):
            source_x = min(width - 1, (x * width + OUT_WIDTH // 2) // OUT_WIDTH)
            offset = (source_y * width + source_x) * channels
            dither = BAYER[y & 3][x & 3] - 8
            red = quantize(source[offset], 8, dither * 2)
            green = quantize(source[offset + 1], 8, dither * 2)
            blue = quantize(source[offset + 2], 4, dither * 4)
            result[y * OUT_WIDTH + x] = (red << 5) | (green << 2) | blue
    return result


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else \
        "assets/sapote-first-environment-wallpaper.png"
    destination = sys.argv[2] if len(sys.argv) > 2 else "build/wallpaper.spw"
    width, height, channels, pixels = read_png(source)
    blob = bytearray(MAGIC)
    blob += struct.pack("<HHH", OUT_WIDTH, OUT_HEIGHT, 256)
    blob += palette()
    blob += build_indices(pixels, width, height, channels)
    with open(destination, "wb") as handle:
        handle.write(blob)
    print(f"{source}: {width}x{height} -> {OUT_WIDTH}x{OUT_HEIGHT}, "
          f"{len(blob)} bytes -> {destination}", file=sys.stderr)


if __name__ == "__main__":
    main()

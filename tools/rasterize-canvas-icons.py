#!/usr/bin/env python3
"""Rasterize the pinned Lucide Canvas tools as deterministic SCI1 alpha."""

from __future__ import annotations

import math
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib


NAMES = ("paintbrush", "slash", "square", "circle", "eraser")
SIZE = 24
SAMPLES = 4
TOKEN = re.compile(
    r"[AaCcHhLlMmVvZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?"
)
PARAMETERS = {"A": 7, "C": 6, "H": 1, "L": 2, "M": 2, "V": 1}


def arc_points(
    start: tuple[float, float], values: list[float]
) -> list[tuple[float, float]]:
    rx, ry, rotation, large_arc, sweep, end_x, end_y = values
    rx = abs(rx)
    ry = abs(ry)
    if rx == 0.0 or ry == 0.0 or start == (end_x, end_y):
        return [(end_x, end_y)]

    angle = math.radians(rotation % 360.0)
    cosine = math.cos(angle)
    sine = math.sin(angle)
    half_x = (start[0] - end_x) / 2.0
    half_y = (start[1] - end_y) / 2.0
    prime_x = cosine * half_x + sine * half_y
    prime_y = -sine * half_x + cosine * half_y
    radius_scale = prime_x * prime_x / (rx * rx)
    radius_scale += prime_y * prime_y / (ry * ry)
    if radius_scale > 1.0:
        scale = math.sqrt(radius_scale)
        rx *= scale
        ry *= scale

    numerator = (
        rx * rx * ry * ry
        - rx * rx * prime_y * prime_y
        - ry * ry * prime_x * prime_x
    )
    denominator = (
        rx * rx * prime_y * prime_y
        + ry * ry * prime_x * prime_x
    )
    coefficient = math.sqrt(max(0.0, numerator / denominator))
    if bool(large_arc) == bool(sweep):
        coefficient = -coefficient
    center_prime_x = coefficient * rx * prime_y / ry
    center_prime_y = -coefficient * ry * prime_x / rx
    center_x = (
        cosine * center_prime_x
        - sine * center_prime_y
        + (start[0] + end_x) / 2.0
    )
    center_y = (
        sine * center_prime_x
        + cosine * center_prime_y
        + (start[1] + end_y) / 2.0
    )

    def vector_angle(
        first: tuple[float, float], second: tuple[float, float]
    ) -> float:
        cross = first[0] * second[1] - first[1] * second[0]
        dot = first[0] * second[0] + first[1] * second[1]
        return math.atan2(cross, dot)

    first = (
        (prime_x - center_prime_x) / rx,
        (prime_y - center_prime_y) / ry,
    )
    second = (
        (-prime_x - center_prime_x) / rx,
        (-prime_y - center_prime_y) / ry,
    )
    start_angle = vector_angle((1.0, 0.0), first)
    delta = vector_angle(first, second)
    if not sweep and delta > 0.0:
        delta -= math.tau
    elif sweep and delta < 0.0:
        delta += math.tau
    steps = max(2, math.ceil(abs(delta) * max(rx, ry) * 2.0))
    points = []
    for index in range(1, steps + 1):
        theta = start_angle + delta * index / steps
        points.append((
            center_x + cosine * rx * math.cos(theta)
            - sine * ry * math.sin(theta),
            center_y + sine * rx * math.cos(theta)
            + cosine * ry * math.sin(theta),
        ))
    points[-1] = (end_x, end_y)
    return points


def path_points(description: str) -> list[list[tuple[float, float]]]:
    tokens = TOKEN.findall(description.replace(",", " "))
    paths: list[list[tuple[float, float]]] = []
    current = (0.0, 0.0)
    beginning = current
    command = ""
    index = 0
    first_move = False

    while index < len(tokens):
        if tokens[index].isalpha():
            command = tokens[index]
            index += 1
            if command.upper() == "Z":
                if paths[-1][-1] != beginning:
                    paths[-1].append(beginning)
                current = beginning
                command = ""
                continue
            first_move = command.upper() == "M"
        if not command:
            raise ValueError(f"missing SVG path command in {description!r}")
        upper = command.upper()
        count = PARAMETERS.get(upper)
        if count is None or index + count > len(tokens):
            raise ValueError(f"unsupported or incomplete SVG path {description!r}")
        values = [float(value) for value in tokens[index:index + count]]
        index += count
        relative = command.islower()

        if upper == "M":
            point = (values[0], values[1])
            if relative:
                point = (current[0] + point[0], current[1] + point[1])
            current = point
            if first_move:
                paths.append([current])
                beginning = current
                first_move = False
            else:
                paths[-1].append(current)
            command = "l" if relative else "L"
        elif upper == "L":
            point = (values[0], values[1])
            if relative:
                point = (current[0] + point[0], current[1] + point[1])
            paths[-1].append(point)
            current = point
        elif upper == "H":
            coordinate = values[0] + current[0] if relative else values[0]
            current = (coordinate, current[1])
            paths[-1].append(current)
        elif upper == "V":
            coordinate = values[0] + current[1] if relative else values[0]
            current = (current[0], coordinate)
            paths[-1].append(current)
        elif upper == "C":
            points = [
                (values[offset], values[offset + 1]) for offset in (0, 2, 4)
            ]
            if relative:
                points = [
                    (current[0] + x, current[1] + y) for x, y in points
                ]
            control_a, control_b, end = points
            start = current
            for step in range(1, 33):
                amount = step / 32.0
                inverse = 1.0 - amount
                paths[-1].append((
                    inverse ** 3 * start[0]
                    + 3.0 * inverse * inverse * amount * control_a[0]
                    + 3.0 * inverse * amount * amount * control_b[0]
                    + amount ** 3 * end[0],
                    inverse ** 3 * start[1]
                    + 3.0 * inverse * inverse * amount * control_a[1]
                    + 3.0 * inverse * amount * amount * control_b[1]
                    + amount ** 3 * end[1],
                ))
            current = end
        elif upper == "A":
            if relative:
                values[5] += current[0]
                values[6] += current[1]
            additions = arc_points(current, values)
            paths[-1].extend(additions)
            current = additions[-1]
    return paths


def rounded_rectangle(element: ET.Element) -> list[tuple[float, float]]:
    x = float(element.attrib.get("x", "0"))
    y = float(element.attrib.get("y", "0"))
    width = float(element.attrib["width"])
    height = float(element.attrib["height"])
    radius = min(
        float(element.attrib.get("rx", "0")), width / 2.0, height / 2.0
    )
    points: list[tuple[float, float]] = []
    for center_x, center_y, first_angle in (
        (x + width - radius, y + radius, -math.pi / 2.0),
        (x + width - radius, y + height - radius, 0.0),
        (x + radius, y + height - radius, math.pi / 2.0),
        (x + radius, y + radius, math.pi),
    ):
        for step in range(9):
            angle = first_angle + math.pi * step / 16.0
            points.append((
                center_x + radius * math.cos(angle),
                center_y + radius * math.sin(angle),
            ))
    points.append(points[0])
    return points


def read_icon(source: Path) -> list[list[tuple[float, float]]]:
    root = ET.parse(source).getroot()
    if root.attrib.get("viewBox") != "0 0 24 24":
        raise ValueError(f"{source}: expected a 24x24 viewBox")
    paths: list[list[tuple[float, float]]] = []
    for element in root:
        kind = element.tag.rsplit("}", 1)[-1]
        if kind == "path":
            paths.extend(path_points(element.attrib["d"]))
        elif kind == "circle":
            center_x = float(element.attrib["cx"])
            center_y = float(element.attrib["cy"])
            radius = float(element.attrib["r"])
            points = [
                (
                    center_x + radius * math.cos(math.tau * index / 128.0),
                    center_y + radius * math.sin(math.tau * index / 128.0),
                )
                for index in range(129)
            ]
            paths.append(points)
        elif kind == "rect":
            paths.append(rounded_rectangle(element))
        else:
            raise ValueError(f"{source}: unsupported SVG element {kind!r}")
    return paths


def squared_distance(
    point: tuple[float, float],
    start: tuple[float, float],
    end: tuple[float, float],
) -> float:
    delta_x = end[0] - start[0]
    delta_y = end[1] - start[1]
    length = delta_x * delta_x + delta_y * delta_y
    if length == 0.0:
        return (point[0] - start[0]) ** 2 + (point[1] - start[1]) ** 2
    amount = (
        (point[0] - start[0]) * delta_x
        + (point[1] - start[1]) * delta_y
    ) / length
    amount = max(0.0, min(1.0, amount))
    nearest = (start[0] + amount * delta_x, start[1] + amount * delta_y)
    return (point[0] - nearest[0]) ** 2 + (point[1] - nearest[1]) ** 2


def rasterize(paths: list[list[tuple[float, float]]]) -> bytes:
    segments = [
        (path[index - 1], path[index])
        for path in paths
        for index in range(1, len(path))
    ]
    alpha = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            covered = 0
            for sample_y in range(SAMPLES):
                for sample_x in range(SAMPLES):
                    point = (
                        x + (sample_x + 0.5) / SAMPLES,
                        y + (sample_y + 0.5) / SAMPLES,
                    )
                    if any(
                        squared_distance(point, start, end) <= 1.0
                        for start, end in segments
                    ):
                        covered += 1
            alpha.append(
                (covered * 255 + SAMPLES * SAMPLES // 2)
                // (SAMPLES * SAMPLES)
            )
    return bytes(alpha)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: rasterize-canvas-icons.py LUCIDE_DIR OUTPUT.a8"
        )
    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    payload = b"".join(
        rasterize(read_icon(source / f"{name}.svg")) for name in NAMES
    )
    checksum = zlib.crc32(payload)
    header = struct.pack(
        "<4s4BII", b"SCI1", 1, SIZE, SIZE, len(NAMES),
        len(payload), checksum,
    )
    destination.write_bytes(header + payload)
    print(
        f"{source}: {len(NAMES)} Lucide icons, {SIZE}x{SIZE} alpha, "
        f"CRC32 {checksum:08X} -> {destination}"
    )


if __name__ == "__main__":
    main()

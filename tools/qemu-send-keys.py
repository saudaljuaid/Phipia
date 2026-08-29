#!/usr/bin/env python3
"""Send text through QEMU's hardware keyboard after a serial marker."""

from __future__ import annotations

import argparse
from pathlib import Path
import socket
import subprocess
import sys
import time


def wait_for_marker(path: Path, marker: bytes, count: int, deadline: float) -> None:
    while time.monotonic() < deadline:
        try:
            if path.read_bytes().count(marker) >= count:
                return
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise TimeoutError(f"serial marker did not appear: {marker.decode('ascii')}")


def connect_monitor(path: str, deadline: float) -> socket.socket:
    address: str | tuple[str, int]
    family: socket.AddressFamily

    if path.startswith("tcp:"):
        host, separator, port_text = path[4:].rpartition(":")
        if not separator or not host:
            raise ValueError("TCP monitor must be tcp:host:port")
        address = (host, int(port_text))
        family = socket.AF_INET
    else:
        address = path
        family = socket.AF_UNIX
    while time.monotonic() < deadline:
        connection = socket.socket(family, socket.SOCK_STREAM)
        try:
            connection.connect(address)
            return connection
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            connection.close()
            time.sleep(0.05)
    raise TimeoutError("QEMU monitor socket did not become ready")


def key_name(character: str) -> str:
    if "a" <= character <= "z" or "0" <= character <= "9":
        return character
    if character == "\n":
        return "ret"
    if character == " ":
        return "spc"
    raise ValueError(f"unsupported injected character: {character!r}")


def send_hmp(monitor: socket.socket, command: str) -> None:
    if "\n" in command or "\r" in command:
        raise ValueError("HMP command contains a line break")
    monitor.sendall((command + "\n").encode("ascii"))


def wait_for_file(path: Path, deadline: float) -> None:
    while time.monotonic() < deadline:
        if path.is_file() and path.stat().st_size > 0:
            return
        time.sleep(0.02)
    raise TimeoutError(f"QEMU did not write screendump: {path}")


def encode_canvas_evidence(ffmpeg: str, frames: list[Path], fps: int,
                           screenshot: Path, video: Path) -> None:
    screenshot.parent.mkdir(parents=True, exist_ok=True)
    video.parent.mkdir(parents=True, exist_ok=True)
    for output in (screenshot, video):
        output.unlink(missing_ok=True)
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(frames[-1]), "-frames:v", "1", str(screenshot),
    ], check=True)
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-framerate", str(fps), "-start_number", "0",
        "-i", str(frames[0].parent / "frame-%04d.ppm"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags",
        "+faststart", str(video),
    ], check=True)
    if screenshot.stat().st_size == 0 or video.stat().st_size == 0:
        raise RuntimeError("encoded native Canvas evidence is empty")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--monitor", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--marker-count", type=int, default=1)
    parser.add_argument("--text", default="")
    parser.add_argument("--enter", action="store_true")
    parser.add_argument("--hmp", action="append", default=[])
    parser.add_argument("--capture-dir", type=Path)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--video", type=Path)
    parser.add_argument("--frames", type=int, default=24)
    parser.add_argument("--fps", type=int, default=8)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--timeout", type=float, default=40.0)
    args = parser.parse_args()
    if args.marker_count < 1:
        parser.error("--marker-count must be positive")
    evidence_values = (args.capture_dir, args.screenshot, args.video)
    if any(value is not None for value in evidence_values) and not all(
            value is not None for value in evidence_values):
        parser.error("canvas evidence requires capture-dir, screenshot, and video")
    if args.frames < 2 or args.fps < 1:
        parser.error("canvas evidence needs at least two frames and a positive fps")
    deadline = time.monotonic() + args.timeout
    wait_for_marker(Path(args.serial), args.marker.encode("ascii"),
                    args.marker_count, deadline)
    with connect_monitor(args.monitor, deadline) as monitor:
        text = args.text + ("\n" if args.enter else "")
        for character in text:
            command = f"sendkey {key_name(character)}\n".encode("ascii")
            monitor.sendall(command)
            time.sleep(0.04)
        for command in args.hmp:
            send_hmp(monitor, command)
            time.sleep(0.08)
        frames: list[Path] = []
        if args.capture_dir is not None:
            capture_dir = args.capture_dir.resolve()
            capture_dir.mkdir(parents=True, exist_ok=True)
            for stale in capture_dir.glob("frame-*.ppm"):
                stale.unlink()
            interval = 1.0 / args.fps
            for index in range(args.frames):
                started = time.monotonic()
                frame = capture_dir / f"frame-{index:04d}.ppm"
                send_hmp(monitor, f"screendump {frame}")
                wait_for_file(frame, min(deadline, started + 2.0))
                frames.append(frame)
                remaining = interval - (time.monotonic() - started)
                if remaining > 0:
                    time.sleep(remaining)
    if args.capture_dir is not None:
        encode_canvas_evidence(args.ffmpeg, frames, args.fps,
                               args.screenshot.resolve(), args.video.resolve())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, TimeoutError,
            UnicodeError, ValueError) as error:
        print(f"QEMU input injection failed: {error}", file=sys.stderr)
        raise SystemExit(1)

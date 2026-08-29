#!/usr/bin/env python3
"""Send text through QEMU's hardware keyboard after a serial marker."""

from __future__ import annotations

import argparse
from pathlib import Path
import socket
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
    while time.monotonic() < deadline:
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.connect(path)
            return connection
        except (FileNotFoundError, ConnectionRefusedError):
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--monitor", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--marker-count", type=int, default=1)
    parser.add_argument("--text", default="")
    parser.add_argument("--enter", action="store_true")
    parser.add_argument("--hmp", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=40.0)
    args = parser.parse_args()
    if args.marker_count < 1:
        parser.error("--marker-count must be positive")
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
            if "\n" in command or "\r" in command:
                raise ValueError("HMP command contains a line break")
            monitor.sendall((command + "\n").encode("ascii"))
            time.sleep(0.08)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, TimeoutError, UnicodeError, ValueError) as error:
        print(f"QEMU input injection failed: {error}", file=sys.stderr)
        raise SystemExit(1)

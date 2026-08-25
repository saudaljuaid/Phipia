#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture the running Sapote First Environment from QEMU.

The recording begins only after the installed Boot Ledger proof and the shell
prompt.  Every frame comes from QEMU's emulated display through QMP.  Pointer
clicks and keystrokes travel through the ordinary PS/2 guest input path, and
the attached FAT32 data image is a durable copy retained beside the evidence.
"""

import argparse
import json
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path


PROOF_LINE = b"Sapote: Boot Ledger installed proof passed"
PROMPT = b"sap> "
WIDTH = 1024
HEIGHT = 768


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(
        ">I", zlib.crc32(kind + body) & 0xFFFFFFFF
    )


def ppm_to_png(source, destination):
    data = Path(source).read_bytes()
    tokens = []
    position = 0
    while len(tokens) < 4:
        while position < len(data) and data[position] in b" \t\r\n":
            position += 1
        if position < len(data) and data[position] == ord("#"):
            newline = data.find(b"\n", position)
            if newline < 0:
                raise RuntimeError("QEMU PPM comment is unterminated")
            position = newline + 1
            continue
        end = position
        while end < len(data) and data[end] not in b" \t\r\n":
            end += 1
        tokens.append(data[position:end])
        position = end
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise RuntimeError("QEMU screendump is not an 8-bit binary PPM")
    width, height = int(tokens[1]), int(tokens[2])
    while position < len(data) and data[position] in b" \t\r\n":
        position += 1
    pixels = data[position:]
    if len(pixels) != width * height * 3:
        raise RuntimeError("QEMU screendump pixel body is truncated")
    rows = b"".join(
        b"\x00" + pixels[y * width * 3:(y + 1) * width * 3]
        for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(
        ">IIBBBBB", width, height, 8, 2, 0, 0, 0
    ))
    png += png_chunk(b"IDAT", zlib.compress(rows, 9))
    png += png_chunk(b"IEND", b"")
    Path(destination).write_bytes(png)


class Qmp:
    def __init__(self, port):
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.socket = socket.create_connection(
                    ("127.0.0.1", port), 0.5
                )
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("QMP did not accept a connection")
                time.sleep(0.05)
        self.file = self.socket.makefile("rwb", buffering=0)
        self._read_message()
        self.execute("qmp_capabilities")

    def _read_message(self):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode("ascii") + b"\r\n")
        response = self._read_message()
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']}")
        return response.get("return")

    def hmp(self, command):
        return self.execute(
            "human-monitor-command", {"command-line": command}
        )

    def close(self):
        self.file.close()
        self.socket.close()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_serial(path, markers, timeout=45.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        transcript = path.read_bytes() if path.exists() else b""
        if all(marker in transcript for marker in markers):
            return
        time.sleep(0.05)
    transcript = path.read_bytes() if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(f"guest readiness markers were omitted\n{tail}")


def storage_arguments(system, data):
    return [
        "-blockdev",
        f"driver=file,filename={system.resolve()},node-name=system-file,read-only=on,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=system-file,node-name=system-raw,read-only=on",
        "-device",
        "nvme,serial=sapote-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
        "-blockdev",
        f"driver=file,filename={data.resolve()},node-name=data-file,read-only=off,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=data-file,node-name=data-raw,read-only=off",
        "-device",
        "nvme,serial=sapote-data-fat32,drive=data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
    ]


def capture_ppm(qmp, destination):
    qmp.execute("screendump", {
        "filename": destination.resolve().as_posix(), "format": "ppm"
    })


def capture_png(qmp, work, output, name):
    ppm = work / f"{name}.ppm"
    destination = output / f"{name}.png"
    capture_ppm(qmp, ppm)
    ppm_to_png(ppm, destination)
    ppm.unlink()


def send_text(qmp, text, delay=0.07):
    names = {" ": "spc", "-": "minus", ".": "dot", "/": "slash"}
    for character in text:
        key = names.get(character, character)
        qmp.hmp(f"sendkey {key}")
        time.sleep(delay)


class Pointer:
    def __init__(self, qmp):
        self.qmp = qmp
        self.x = WIDTH - WIDTH // 4
        self.y = HEIGHT // 3

    def move_to(self, x, y):
        while self.x != x or self.y != y:
            dx = max(-40, min(40, x - self.x))
            dy = max(-40, min(40, y - self.y))
            self.qmp.hmp(f"mouse_move {dx} {dy}")
            self.x += dx
            self.y += dy
            time.sleep(0.08)
        time.sleep(0.30)

    def prime_terminal(self):
        # QEMU's relative PS/2 path needs one ordinary large host motion before
        # it begins emitting the smaller packets used for the scripted path.
        self.qmp.hmp("mouse_move -260 320")
        time.sleep(0.45)
        self.qmp.hmp("mouse_move 4 120")
        self.x = 512
        self.y = 696
        time.sleep(1.10)

    def click(self):
        self.qmp.hmp("mouse_button 1")
        time.sleep(0.10)
        self.qmp.hmp("mouse_button 0")
        time.sleep(0.18)


def encode(ffmpeg, pattern, fps, seconds, output):
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-framerate", str(fps), "-i", str(pattern),
        "-vf", "format=yuv420p", "-c:v", "libx264", "-preset", "medium",
        "-crf", "18", "-movflags", "+faststart", "-t",
        f"{seconds:.3f}", str(output)
    ], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--system", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--fps", type=int, default=8)
    args = parser.parse_args()
    if args.seconds < 20.0:
        parser.error("the First Environment proof needs at least 20 seconds")
    if args.fps <= 0:
        parser.error("--fps must be positive")

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    durable_data = output / "sapote-first-environment-data.raw"
    shutil.copyfile(Path(args.data).resolve(), durable_data)
    serial = output / "sapote-first-environment-serial.log"
    if serial.exists():
        serial.unlink()
    video = output / "sapote-first-environment-20s.mp4"
    port = free_port()
    command = [
        args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(Path(args.iso).resolve()),
        "-display", "none",
        *storage_arguments(Path(args.system).resolve(), durable_data),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"
    ]

    with tempfile.TemporaryDirectory(prefix="sapote-first-environment-") as raw:
        work = Path(raw)
        process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        qmp = None
        try:
            qmp = Qmp(port)
            wait_serial(serial, (PROOF_LINE, PROMPT))
            time.sleep(0.35)
            pointer = Pointer(qmp)
            capture_png(qmp, work, output, "sapote-first-environment")

            frame_count = round(args.seconds * args.fps)
            events = set()
            started = time.monotonic()
            for index in range(frame_count):
                deadline = started + index / args.fps
                remaining = deadline - time.monotonic()
                if remaining > 0.0:
                    time.sleep(remaining)
                elapsed = index / args.fps

                if elapsed >= 0.75 and "terminal_hover" not in events:
                    pointer.prime_terminal()
                    capture_png(qmp, work, output,
                                "sapote-first-environment-dock")
                    events.add("terminal_hover")
                if elapsed >= 2.50 and "terminal_open" not in events:
                    pointer.click()
                    time.sleep(0.35)
                    events.add("terminal_open")
                if elapsed >= 3.25 and "fetch" not in events:
                    send_text(qmp, "fetch")
                    qmp.hmp("sendkey ret")
                    time.sleep(0.75)
                    capture_png(qmp, work, output,
                                "sapote-first-environment-terminal")
                    events.add("fetch")
                if elapsed >= 6.75 and "terminal_close" not in events:
                    pointer.move_to(101, 57)
                    pointer.click()
                    events.add("terminal_close")
                if elapsed >= 7.75 and "files_open" not in events:
                    pointer.move_to(394, 700)
                    pointer.click()
                    time.sleep(0.35)
                    events.add("files_open")
                if elapsed >= 9.75 and "new_file" not in events:
                    pointer.move_to(212, 99)
                    pointer.click()
                    time.sleep(0.45)
                    capture_png(qmp, work, output,
                                "sapote-first-environment-files-new")
                    events.add("new_file")
                if elapsed >= 11.25 and "file_open" not in events:
                    pointer.move_to(363, 180)
                    pointer.click()
                    time.sleep(0.80)
                    events.add("file_open")
                if elapsed >= 12.25 and "note_saved" not in events:
                    send_text(qmp, "first cut / saved from files.")
                    time.sleep(0.40)
                    pointer.move_to(138, 99)
                    pointer.click()
                    qmp.hmp("sendkey ctrl-s")
                    time.sleep(0.80)
                    capture_png(qmp, work, output,
                                "sapote-first-environment-notes")
                    events.add("note_saved")
                if elapsed >= 15.75 and "notes_close" not in events:
                    pointer.move_to(101, 57)
                    pointer.click()
                    events.add("notes_close")
                if elapsed >= 16.75 and "files_reopen" not in events:
                    pointer.move_to(394, 700)
                    pointer.click()
                    time.sleep(0.40)
                    capture_png(qmp, work, output,
                                "sapote-first-environment-files")
                    events.add("files_reopen")

                capture_ppm(qmp, work / f"frame-{index:04d}.ppm")

            required = {
                "terminal_hover", "terminal_open", "fetch", "terminal_close",
                "files_open", "new_file", "file_open", "note_saved",
                "notes_close", "files_reopen"
            }
            if events != required:
                raise RuntimeError(f"capture omitted interactions: {required - events}")
        finally:
            if qmp is not None:
                try:
                    qmp.execute("quit")
                except (OSError, RuntimeError):
                    pass
                qmp.close()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

        transcript = serial.read_bytes() if serial.exists() else b""
        if PROOF_LINE not in transcript or b"Sapote First Environment" not in transcript:
            tail = transcript[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError("recording omitted proof or fetch output\n" + tail)
        encode(args.ffmpeg, work / "frame-%04d.ppm", args.fps,
               args.seconds, video)

    print(video)


if __name__ == "__main__":
    main()

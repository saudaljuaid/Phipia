#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Record a fixed-duration Sapote boot from QEMU's emulated display.

Frames come directly from QMP ``screendump`` calls. The guest is started in a
paused state, continued on the first frame, and required to emit the installed
Boot Ledger proof before the recording is accepted. At ten seconds the script
opens Terminal; at thirteen seconds it enters ``version`` so the finished clip
also proves that the newly booted desktop is interactive.
"""

import argparse
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path


PROOF_LINE = b"Sapote: Boot Ledger installed proof passed"


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


def capture(qmp, destination):
    qmp.execute("screendump", {
        "filename": destination.resolve().as_posix(), "format": "ppm"
    })


def encode(ffmpeg, pattern, fps, seconds, output):
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-framerate", str(fps), "-i", str(pattern),
        "-vf",
        "scale=1024:768:force_original_aspect_ratio=decrease:flags=neighbor,"
        "pad=1024:768:(ow-iw)/2:(oh-ih)/2:black,format=yuv420p",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-movflags", "+faststart", "-t", f"{seconds:.3f}", str(output)
    ]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--fps", type=int, default=10)
    args = parser.parse_args()

    if args.seconds <= 0.0 or args.fps <= 0:
        raise ValueError("seconds and fps must be positive")
    frame_count = round(args.seconds * args.fps)
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="sapote-boot-video-") as work:
        work = Path(work)
        serial = work / "boot-serial.log"
        port = free_port()
        command = [
            args.qemu, "-S", "-machine", "accel=tcg", "-m", "128M",
            "-smp", "1", "-cdrom", str(Path(args.iso).resolve()),
            "-display", "none",
            "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
            "-serial", f"file:{serial}", "-no-reboot"
        ]
        process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        qmp = None
        try:
            qmp = Qmp(port)
            started = time.monotonic()
            qmp.execute("cont")
            terminal_opened = False
            version_entered = False
            for index in range(frame_count):
                deadline = started + index / args.fps
                remaining = deadline - time.monotonic()
                if remaining > 0.0:
                    time.sleep(remaining)
                elapsed = index / args.fps
                if elapsed >= 10.0 and not terminal_opened:
                    qmp.hmp("sendkey ret")
                    terminal_opened = True
                if elapsed >= 13.0 and not version_entered:
                    for key in "version":
                        qmp.hmp(f"sendkey {key}")
                    qmp.hmp("sendkey ret")
                    version_entered = True
                capture(qmp, work / f"frame-{index:04d}.ppm")
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
        if PROOF_LINE not in transcript:
            tail = transcript[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError(
                "recording omitted the installed Boot Ledger proof\n" + tail
            )
        encode(
            args.ffmpeg, work / "frame-%04d.ppm", args.fps,
            args.seconds, output
        )
    print(output)


if __name__ == "__main__":
    main()

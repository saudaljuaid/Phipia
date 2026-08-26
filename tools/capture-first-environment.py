#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture the running Sapote First Environment from QEMU.

The recording begins only after the installed Boot Ledger proof and the shell
prompt.  Frames come from QEMU's emulated display through QMP, or from its
visible SDL client in live-window mode.  Pointer clicks and keystrokes travel
through the ordinary PS/2 guest input path, and the attached FAT32 data image
is a durable copy retained beside the evidence.
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import fat32_image


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
                self.socket.settimeout(None)
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


def wait_for_export(data_image, timeout=20.0):
    expected_size = 54 + 320 * 180 * 3
    deadline = time.monotonic() + timeout
    last_error = "EXPORT.BMP was not visible"

    # Let the ordinary guest NVMe path finish without competing with repeated
    # 64 MiB host reads while its FAT and FSInfo updates are in flight.
    time.sleep(8.0)
    while time.monotonic() < deadline:
        try:
            report = fat32_image.inspect_image(data_image.read_bytes())
            files = {
                str(record["path"]): record
                for record in report["files"]
                if not bool(record["directory"])
            }
            exported = files.get("EXPORT.BMP")
            if exported is not None and int(exported["size"]) == expected_size:
                return
            last_error = "EXPORT.BMP was absent or incomplete"
        except (OSError, fat32_image.Fat32Error) as error:
            last_error = str(error)
        time.sleep(5.0)
    raise RuntimeError(f"guest export did not synchronize: {last_error}")


def click_export_and_wait(pointer, data_image):
    last_error = None
    for _ in range(3):
        pointer.click()
        try:
            wait_for_export(data_image)
            return
        except RuntimeError as error:
            last_error = error
    raise last_error


def wait_until(started, seconds):
    remaining = started + seconds - time.monotonic()
    if remaining > 0.0:
        time.sleep(remaining)


def finish_recording(process):
    try:
        _, errors = process.communicate(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        _, errors = process.communicate()
        raise RuntimeError("live QEMU recording did not stop at its bound")
    if process.returncode != 0:
        raise RuntimeError("live QEMU recording failed\n" + errors[-4096:])


def prepare_live_window(title):
    import ctypes
    from ctypes import wintypes

    class Rect(ctypes.Structure):
        _fields_ = [
            ("left", ctypes.c_long), ("top", ctypes.c_long),
            ("right", ctypes.c_long), ("bottom", ctypes.c_long),
        ]

    class Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    user = ctypes.windll.user32
    deadline = time.monotonic() + 10.0
    handle = 0
    while not handle and time.monotonic() < deadline:
        handle = user.FindWindowW(None, title)
        if not handle:
            time.sleep(0.05)
    if not handle:
        raise RuntimeError(f"QEMU live window was not found: {title}")

    user.ShowWindow(handle, 9)  # SW_RESTORE
    user.SetForegroundWindow(handle)
    # A decorated 1024x768 SDL client is taller than the usable area on some
    # Windows desktops.  The dedicated evidence window is borderless so the
    # guest framebuffer remains unscaled and no host chrome enters the video.
    popup_style = ctypes.c_long(0x90000000).value  # WS_POPUP | WS_VISIBLE
    user.SetWindowLongW(handle, -16, popup_style)  # GWL_STYLE
    stable = 0
    observed = (0, 0)
    for _ in range(60):
        window = Rect()
        client = Rect()
        if not user.GetWindowRect(handle, ctypes.byref(window)) or not \
                user.GetClientRect(handle, ctypes.byref(client)):
            raise RuntimeError("QEMU live window geometry is unavailable")
        client_width = client.right - client.left
        client_height = client.bottom - client.top
        observed = (client_width, client_height)
        if client_width == WIDTH and client_height == HEIGHT:
            stable += 1
            if stable == 3:
                break
            time.sleep(0.15)
            continue
        stable = 0
        if not user.SetWindowPos(handle, 0, 80, 0, WIDTH, HEIGHT,
                                 0x0004 | 0x0020 | 0x0040):
            raise RuntimeError("QEMU live window could not be sized")
        time.sleep(0.15)
    else:
        raise RuntimeError(
            "QEMU live client did not settle at 1024x768 "
            f"(last {observed[0]}x{observed[1]})"
        )

    window = Rect()
    client = Rect()
    origin = Point(0, 0)
    user.GetWindowRect(handle, ctypes.byref(window))
    user.GetClientRect(handle, ctypes.byref(client))
    user.ClientToScreen(handle, ctypes.byref(origin))
    if client.right - client.left != WIDTH or client.bottom - client.top != HEIGHT:
        raise RuntimeError("QEMU live client changed after sizing")
    return origin.x - window.left, origin.y - window.top


def record_live_window(args, qmp, pointer, work, output, durable_data, video,
                       crop):
    title = "QEMU (SapoteCapture-0)"
    crop_x, crop_y = crop
    command = [
        args.ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "gdigrab", "-draw_mouse", "0", "-framerate", str(args.fps),
        "-i", f"title={title}", "-t", f"{args.seconds:.3f}",
        "-vf", f"crop={WIDTH}:{HEIGHT}:{crop_x}:{crop_y},format=yuv420p",
        "-c:v", "libx264", "-preset", "medium",
        "-crf", "18", "-movflags", "+faststart", str(video),
    ]
    recording = subprocess.Popen(
        command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.75)
    if recording.poll() is not None:
        finish_recording(recording)
    started = time.monotonic()

    try:
        wait_until(started, 0.25)
        pointer.prime_terminal()
        pointer.click()
        wait_until(started, 1.35)
        send_text(qmp, "fetch")
        qmp.hmp("sendkey ret")
        wait_until(started, 3.20)
        capture_png(qmp, work, output, "sapote-first-environment-terminal")

        wait_until(started, 3.80)
        pointer.move_to(101, 57)
        pointer.click()
        wait_until(started, 4.65)
        pointer.move_to(368, 700)
        pointer.click()
        wait_until(started, 5.75)
        pointer.move_to(212, 99)
        pointer.click()
        wait_until(started, 6.55)
        capture_png(qmp, work, output, "sapote-first-environment-files-new")
        capture_png(qmp, work, output, "sapote-first-environment-files")

        wait_until(started, 7.10)
        pointer.move_to(525, 180)
        pointer.click()
        wait_until(started, 8.00)
        qmp.hmp("sendkey left 15")
        time.sleep(0.20)
        send_text(qmp, "first cut / saved in Sapote.")
        wait_until(started, 9.10)
        pointer.move_to(138, 99)
        pointer.click()
        qmp.hmp("sendkey ctrl-s")
        wait_until(started, 10.80)
        capture_png(qmp, work, output, "sapote-first-environment-notes")

        wait_until(started, 11.25)
        pointer.move_to(101, 57)
        pointer.click()
        wait_until(started, 11.90)
        pointer.move_to(368, 700)
        pointer.click()
        wait_until(started, 12.75)
        pointer.move_to(525, 180)
        pointer.click()
        wait_until(started, 14.10)
        pointer.move_to(101, 57)
        pointer.click()

        wait_until(started, 14.75)
        pointer.move_to(656, 700)
        pointer.click()
        wait_until(started, 15.55)
        pointer.move_to(134, 97)
        pointer.click()
        wait_until(started, 16.00)
        pointer.move_to(206, 97)
        pointer.click()
        wait_until(started, 17.30)
        pointer.move_to(280, 97)
        pointer.click()
        wait_until(started, 17.75)
        pointer.move_to(566, 564)
        pointer.click()
        wait_until(started, 18.20)
        pointer.move_to(350, 97)
        pointer.click()
        wait_until(started, args.seconds)
        finish_recording(recording)

        # Keep the evidence image richer than the short visual demonstration.
        # The recording is already complete, so slow synchronized replacement
        # cannot make the visible cursor stutter.
        time.sleep(5.0)
        pointer.move_to(420, 97)
        click_export_and_wait(pointer, durable_data)
        capture_png(qmp, work, output, "sapote-first-environment-studio")
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.communicate()


def send_text(qmp, text, delay=0.040):
    names = {" ": "spc", "-": "minus", ".": "dot", "/": "slash"}
    for character in text:
        if "A" <= character <= "Z":
            key = f"shift-{character.lower()}"
        else:
            key = names.get(character, character)
        qmp.hmp(f"sendkey {key} 15")
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
            time.sleep(0.025)
        time.sleep(0.10)

    def prime_terminal(self):
        # QEMU's relative PS/2 path needs one ordinary large host motion before
        # it begins emitting the smaller packets used for the scripted path.
        self.qmp.hmp("mouse_move -260 320")
        time.sleep(0.25)
        self.qmp.hmp("mouse_move 4 120")
        self.x = 512
        self.y = 696
        time.sleep(0.45)
        self.move_to(464, 696)

    def click(self):
        self.qmp.hmp("mouse_button 1")
        time.sleep(0.05)
        self.qmp.hmp("mouse_button 0")
        time.sleep(0.08)


def encode(ffmpeg, frames, capture_times, fps, seconds, output):
    if not frames or len(frames) != len(capture_times):
        raise RuntimeError("video frame timing evidence is incomplete")
    origin = capture_times[0]
    normalized = [timestamp - origin for timestamp in capture_times]
    manifest = frames[0].parent / "frames.ffconcat"
    lines = ["ffconcat version 1.0"]
    for index, frame in enumerate(frames):
        if index + 1 < len(frames):
            duration = normalized[index + 1] - normalized[index]
        else:
            duration = seconds - normalized[index]
        duration = max(0.001, duration)
        lines.append(f"file '{frame.resolve().as_posix()}'")
        lines.append(f"duration {duration:.9f}")
    lines.append(f"file '{frames[-1].resolve().as_posix()}'")
    manifest.write_text("\n".join(lines) + "\n", encoding="ascii")
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "concat", "-safe", "0", "-i", str(manifest),
        "-vf", "format=yuv420p,tpad=stop_mode=clone:stop_duration=12",
        "-c:v", "libx264", "-preset", "medium",
        "-crf", "18", "-r", str(fps), "-movflags", "+faststart", "-t",
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
    parser.add_argument(
        "--live-window", action="store_true",
        help="record the visible Windows QEMU SDL window with gdigrab",
    )
    args = parser.parse_args()
    if args.seconds < 20.0:
        parser.error("the First Environment proof needs at least 20 seconds")
    if args.fps <= 0:
        parser.error("--fps must be positive")

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    durable_data = output / "sapote-first-environment-data.raw"
    shutil.copyfile(Path(args.data).resolve(), durable_data)
    staged_media = output / ".sapote-capture-aurora.bmp"
    try:
        subprocess.run([
            args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-i", str((Path(__file__).resolve().parent.parent /
                       "assets/sapote-first-environment-wallpaper.png")),
            "-vf", "scale=320:180", "-pix_fmt", "bgr24", "-c:v", "bmp",
            str(staged_media),
        ], check=True)
        populated = fat32_image.populate_data_image(
            durable_data.read_bytes(), "AURORA.BMP", staged_media.read_bytes()
        )
        fat32_image.atomic_write(durable_data, populated)
    finally:
        staged_media.unlink(missing_ok=True)
    serial = output / "sapote-first-environment-serial.log"
    if serial.exists():
        serial.unlink()
    video = output / "sapote-first-environment-20s.mp4"
    port = free_port()
    command = [
        args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(Path(args.iso).resolve()),
        "-display", ("sdl,window-close=off" if args.live_window else "none"),
        *( ["-name", "SapoteCapture"] if args.live_window else [] ),
        *storage_arguments(Path(args.system).resolve(), durable_data),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"
    ]

    with tempfile.TemporaryDirectory(prefix="sapote-first-environment-") as raw:
        work = Path(raw)
        qemu_environment = os.environ.copy()
        if args.live_window:
            # SDL must use physical pixels so gdigrab, Win32 geometry and the
            # 1024x768 guest surface share one coordinate system on scaled
            # Windows desktops.
            qemu_environment["SDL_WINDOWS_DPI_AWARENESS"] = "permonitorv2"
            qemu_environment["SDL_WINDOWS_DPI_SCALING"] = "0"
        process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=qemu_environment,
        )
        qmp = None
        try:
            qmp = Qmp(port)
            live_crop = None
            if args.live_window:
                # Size the SDL client before the guest draws the desktop.  A
                # later host resize leaves regions that Sapote has no reason
                # to repaint and produces misleading partial capture frames.
                live_crop = prepare_live_window("QEMU (SapoteCapture-0)")
            wait_serial(serial, (PROOF_LINE, PROMPT))
            time.sleep(0.35)
            pointer = Pointer(qmp)
            capture_png(qmp, work, output, "sapote-first-environment")

            if args.live_window:
                record_live_window(args, qmp, pointer, work, output,
                                   durable_data, video, live_crop)
                events = {
                    "terminal_hover", "terminal_open", "fetch",
                    "terminal_close", "files_open", "new_file", "file_open",
                    "note_saved", "notes_close", "studio_open", "studio_new",
                    "studio_import", "studio_trim", "studio_seek",
                    "studio_save", "studio_export",
                }
                captured_frames = []
                capture_times = []
            else:
                events = set()
                captured_frames = []
                capture_times = []
                started = time.monotonic()
                next_capture = started
                index = 0
                while time.monotonic() - started < args.seconds:
                    elapsed = time.monotonic() - started

                    if elapsed >= 0.50 and "terminal_hover" not in events:
                        pointer.prime_terminal()
                        capture_png(qmp, work, output,
                                    "sapote-first-environment-dock")
                        events.add("terminal_hover")
                    elif elapsed >= 1.50 and "terminal_open" not in events:
                        pointer.click()
                        time.sleep(0.35)
                        events.add("terminal_open")
                    elif elapsed >= 2.25 and "fetch" not in events:
                        send_text(qmp, "fetch")
                        qmp.hmp("sendkey ret")
                        time.sleep(0.75)
                        capture_png(qmp, work, output,
                                    "sapote-first-environment-terminal")
                        events.add("fetch")
                    elif elapsed >= 4.75 and "terminal_close" not in events:
                        pointer.move_to(101, 57)
                        pointer.click()
                        events.add("terminal_close")
                    elif elapsed >= 5.75 and "files_open" not in events:
                        pointer.move_to(368, 700)
                        pointer.click()
                        time.sleep(0.35)
                        events.add("files_open")
                    elif elapsed >= 7.00 and "new_file" not in events:
                        pointer.move_to(212, 99)
                        pointer.click()
                        time.sleep(1.50)
                        capture_png(qmp, work, output,
                                    "sapote-first-environment-files-new")
                        capture_png(qmp, work, output,
                                    "sapote-first-environment-files")
                        events.add("new_file")
                    elif elapsed >= 8.50 and "file_open" not in events:
                        pointer.move_to(525, 180)
                        pointer.click()
                        time.sleep(0.80)
                        events.add("file_open")
                    elif elapsed >= 10.00 and "note_saved" not in events:
                        send_text(qmp, "first cut / saved from files.")
                        time.sleep(0.40)
                        pointer.move_to(138, 99)
                        pointer.click()
                        qmp.hmp("sendkey ctrl-s")
                        time.sleep(0.80)
                        capture_png(qmp, work, output,
                                    "sapote-first-environment-notes")
                        events.add("note_saved")
                    elif elapsed >= 12.00 and "notes_close" not in events:
                        pointer.move_to(101, 57)
                        pointer.click()
                        events.add("notes_close")
                    elif elapsed >= 12.50 and "studio_open" not in events:
                        pointer.move_to(656, 700)
                        pointer.click()
                        time.sleep(0.40)
                        events.add("studio_open")
                    elif elapsed >= 13.00 and "studio_new" not in events:
                        pointer.move_to(134, 97)
                        pointer.click()
                        time.sleep(0.20)
                        events.add("studio_new")
                    elif elapsed >= 13.35 and "studio_import" not in events:
                        pointer.move_to(206, 97)
                        pointer.click()
                        time.sleep(0.80)
                        events.add("studio_import")
                    elif elapsed >= 14.00 and "studio_trim" not in events:
                        pointer.move_to(280, 97)
                        pointer.click()
                        time.sleep(0.25)
                        events.add("studio_trim")
                    elif elapsed >= 14.35 and "studio_seek" not in events:
                        pointer.move_to(566, 564)
                        pointer.click()
                        time.sleep(0.20)
                        events.add("studio_seek")
                    elif elapsed >= 14.70 and "studio_save" not in events:
                        pointer.move_to(350, 97)
                        pointer.click()
                        events.add("studio_save")

                    now = time.monotonic()
                    if now - started >= args.seconds:
                        break
                    remaining = next_capture - now
                    if remaining > 0.0:
                        time.sleep(remaining)
                    captured_at = time.monotonic()
                    if captured_at - started >= args.seconds:
                        break

                    frame = work / f"frame-{index:04d}.ppm"
                    capture_ppm(qmp, frame)
                    captured_at = time.monotonic()
                    captured_frames.append(frame)
                    capture_times.append(captured_at)
                    index += 1
                    next_capture = captured_at + 1.0 / args.fps

                # The twenty-second recording ends after the visible Save
                # click.  Complete the slower synchronized export afterward,
                # as live-window capture does, while QEMU and its NVMe devices
                # remain active for the consistency evidence.
                time.sleep(5.0)
                pointer.move_to(420, 97)
                click_export_and_wait(pointer, durable_data)
                capture_png(qmp, work, output,
                            "sapote-first-environment-studio")
                events.add("studio_export")

            required = {
                "terminal_hover", "terminal_open", "fetch", "terminal_close",
                "files_open", "new_file", "file_open", "note_saved",
                "notes_close", "studio_open", "studio_new", "studio_import",
                "studio_trim", "studio_seek", "studio_save", "studio_export"
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

        report = fat32_image.inspect_image(durable_data.read_bytes())
        files = {
            str(record["path"]): record
            for record in report["files"]
            if not bool(record["directory"])
        }
        expected_sizes = {
            "AURORA.BMP": 54 + 320 * 180 * 3,
            "SAPSTUDI.SAP": 424,
            "EXPORT.BMP": 54 + 320 * 180 * 3,
        }
        for name, size in expected_sizes.items():
            if name not in files or int(files[name]["size"]) != size:
                raise RuntimeError(
                    f"guest evidence omitted {name} with exact size {size}"
                )
        if "NEW1.TXT" not in files or int(files["NEW1.TXT"]["size"]) == 0:
            raise RuntimeError("guest evidence omitted the saved Notes document")
        if (not bool(report["fat_copies_match"]) or
                int(report["cycles"]) != 0 or
                int(report["cross_links"]) != 0 or
                int(report["leaked_clusters"]) != 0):
            raise RuntimeError("guest evidence left an inconsistent FAT32 image")
        (output / "report.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if not args.live_window:
            encode(args.ffmpeg, captured_frames, capture_times, args.fps,
                   args.seconds, video)

    print(video)


if __name__ == "__main__":
    main()

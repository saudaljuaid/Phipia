<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Sapote First Environment

Sapote First Environment is the current bounded graphical shell. It replaces
the original First Light workspace after the same Boot Ledger, framebuffer,
cached-surface, keyboard, pointer, and filesystem proofs have completed. It is
not a general window manager or application ABI.

## Desktop and Dock

The desktop uses a deterministic 1024×768 photographic wallpaper generated
from the committed source image. The kernel converts it to a bounded indexed
asset at build time, Rust validates its header and checked geometry, and C
draws only decoded pixels through the cached surface.

The Dock owns exactly four applications: Files, Terminal, Notes, and SapStudio.
Its code-native icons sit on a translucent trapezoidal shelf with a bright rim,
dark lower lip, per-icon reflections, active indicators, hover magnification,
and smaller magnification on neighboring icons. These effects use fixed memory
and deterministic damaged rectangles; there is no compositor or theme engine.
SapStudio uses the exact supplied clapperboard artwork, cropped to its primary
transparent component and decoded through the same bounded image path as the
Sapote mark.

The pointer is an 18×25 classic black arrow with a one-pixel light edge. Pointer
movement and buttons arrive through the ordinary PS/2 interrupt path. The
event queue coalesces motion without discarding button transitions, while all
drawing remains in process context.

## Windows

Only one application window is visible at a time. Each has platinum chrome,
a title, and a real red close button. Clicking its X or pressing Escape closes
the window and returns to the wallpaper. Dock clicks select an application;
keyboard focus remains available with Tab and Enter.

## Files

Files is a real view of the writable FAT32 data mount. Its compact toolbar
provides Up, New File, New Folder, Refresh, and Sync. The source-list sidebar
identifies the `data` device, current place, FAT32 format, and read/write state.
The icon grid is populated by `sapfs_list`, and the bottom strip reports the
actual item count and bounded free-space value.

The toolbar exposes only implemented actions; there are no decorative view or
search controls. SapStudio scratch, backup, and project bookkeeping names are
hidden at the data root, while imported media and completed exports remain
visible as ordinary user files.

Creating a file or folder calls the kernel filesystem interface. Opening a
folder traverses it; opening a file hands its real path to Notes. Files never
redirects into the immutable system volume, and every status is recoverable if
the data volume is absent or corrupt.

## Notes

Notes edits one bounded ASCII file buffer. Opening a file reads its bytes from
the data volume. Printable keyboard input, Enter, and Backspace update only the
bounded in-memory buffer. Save or Ctrl-S creates the file if needed, truncates
it, writes the complete buffer, closes its generation-authenticated handle,
and synchronizes the FAT32 volume. Closing a dirty note attempts the same
bounded save. The status line distinguishes unsaved memory from synchronized
data.

## Terminal

Terminal opens on a clean dark surface with a green bitmap palette. It exposes
the existing Sapote shell and therefore retains the native filesystem commands
and the measured `linux echo`, `linux uname`, and `linux cat` profiles. `fetch`
draws the decoded green pebble itself and reports the
kernel, terminal geometry, independent FAT32 mount states, and heap usage.

## SapStudio

SapStudio is a native dark editing workspace modeled on early-2010s
professional non-linear editors. Its source browser, viewer, inspector, track
lanes, clip blocks, playhead, project controls, and close control are drawn by
the kernel shell. New, Import, one-second Trim, timeline selection/seeking,
Save, Export, and Ctrl-S are functional. Import reads an ordinary uncompressed
24-bit BMP from the current Files directory on the writable data volume. The
bounded decoder accepts at most 1920×1080, validates every header and row
offset, and issues random row reads through the normal filesystem and NVMe
paths. Up to six imported paths and their edited durations are retained.

Export writes the selected decoded frame as a standard 24-bit `EXPORT.BMP` at
up to 320×180. It writes and synchronizes a scratch file before replacing any
previous export, restoring the previous output if replacement fails. This is a
real image export, not a claimed time-based codec render. The bounded
`SAPSTUDI.SAP` project record uses the data volume and the same scratch-file,
backup, sync, and recovery discipline as Notes; completed saves
reload after a clean reboot.

The editor foundation is mirrored byte-for-byte from SapStudio's
`engineering-foundation` commit
`70295ebc08a1825452f7c08256aac14270f4cc7b`. It includes the project model,
timecode, deterministic media structures, render graph, compositing, audio,
LUT, EDL, mask, transition, and freestanding-image work. The current native
window deliberately does not claim compressed video decoding, time-based
playback, audio output, or rendered video export: those imported engine modules
are not yet connected to Sapote's graphical ABI.

For host staging, `tools/fat32_image.py populate-data fresh.raw media.raw
--input clip.bmp --name CLIP.BMP` places one bounded 8.3-named file into a
fresh deterministic data image, verifies both FAT copies and FSInfo, and emits
a consistency report. Runtime import and export still use only the ordinary
guest FAT32/NVMe path.

## Authentic evidence

`make capture-first-environment` boots the production ISO in QEMU with the
ordinary read-only system NVMe volume and writable data NVMe volume. QMP sends
real pointer and PS/2 keyboard input to a visible QEMU display. The authentic
approximately 20-second recording opens Terminal and runs `fetch`, creates a
file in Files, types and synchronizes its contents in Notes, closes and reloads
the persisted note, and opens SapStudio. The same uninterrupted guest session
then imports the staged BMP, trims it, saves the project, and exports
`EXPORT.BMP` before the capture tool retains and inspects the data image. The
retained image can be checked with `tools/fat32_image.py inspect`; no
host-rendered UI, pasted note contents, or transcript substitution is involved.

## Retained limits

The environment has four fixed applications, one fixed window, one supported
wallpaper geometry, ASCII text, and no arbitrary application loading. It has no
movable or overlapping windows, desktop settings, networking UI, accessibility
API, international text stack, compressed-media decoder, time-based playback,
audio output, rendered-video export, or stable graphical ABI.
The FAT32 limits and clean-sync persistence boundary remain those documented in
[`FAT32.md`](FAT32.md).

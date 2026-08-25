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

The centered Dock owns exactly three applications: Files, Terminal, and Notes.
Its code-native icons sit on a translucent trapezoidal shelf with a bright rim,
dark lower lip, per-icon reflections, active indicators, hover magnification,
and smaller magnification on neighboring icons. These effects use fixed memory
and deterministic damaged rectangles; there is no compositor or theme engine.

The pointer is a 27×37 classic black arrow with a one-pixel light edge. Pointer
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
draws a green ASCII pebble derived from the canonical mark and reports the
kernel, terminal geometry, independent FAT32 mount states, and heap usage.

## Authentic evidence

`make capture-first-environment` boots the production ISO in QEMU with the
ordinary read-only system NVMe volume and writable data NVMe volume. QMP sends
real pointer and keyboard input, takes screenshots from the emulated display,
and records an approximately 20-second sequence that opens Terminal, runs
`fetch`, creates a file in Files, edits and synchronizes it in Notes, and
returns to the updated Files grid. The retained data image can be checked with
`tools/fat32_image.py inspect`; no host-rendered UI or transcript substitution
is involved.

## Retained limits

The environment has three fixed applications, one fixed window, one supported
wallpaper geometry, ASCII text, and no arbitrary application loading. It has no
movable or overlapping windows, desktop settings, networking UI, accessibility
API, international text stack, multimedia framework, or stable graphical ABI.
The FAT32 limits and clean-sync persistence boundary remain those documented in
[`FAT32.md`](FAT32.md).

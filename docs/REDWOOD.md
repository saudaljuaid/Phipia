<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Sapote Redwood

Sapote Redwood is Sapote's first named graphical release. The bounded shell
starts after the Boot Ledger, framebuffer, cached-surface, keyboard, pointer,
and filesystem proofs have completed. It is not a general window manager or
application ABI.

## Desktop and Dock

The desktop has fourteen deterministic photographic choices: the original
Aurora and thirteen additional scenes. The kernel converts the committed
sources into one bounded indexed album at build time, Rust validates its
header, frame count, and checked geometry, and C decodes only a selected frame.
At the native 1024×768 mode, damage restoration uses row blits from the cached
wallpaper instead of one surface call per pixel.

The Dock owns exactly six applications: Files, Terminal, Notes, SapStudio,
Camera, and Settings. Its interaction and geometry are a native fixed-point
port of the private `saudaljuaid/3d-dock` C implementation: 1.95× raised-cosine
magnification over a 2.75-icon neighborhood, pointer-anchored eased widths,
neighbor displacement, a growing frosted trapezoidal shelf, perspective-warped
reflections, running lights, tooltip fades, press squash, and a decaying launch
bounce. The rest size remains compact enough for a 1024×768 workspace. Dark
appearance changes only the shelf colour; it does not change geometry,
reflections, magnification, windows, or wallpaper.

SapStudio uses the supplied clapperboard artwork. Camera and Settings retain
the supplied classic artwork, with derived alpha-clean dock images that remove
only the surrounding JPEG/PNG canvas. Rectangular icon shadows were removed so
the marks keep clean silhouettes and spacing on the shelf. All three are
converted into checked bounded image blobs and decoded through the same Rust/C
asset boundary as the Sapote mark.

The pointer is an 18×25 classic black arrow with a one-pixel light edge. Pointer
movement and buttons arrive through the ordinary PS/2 interrupt path. The
event queue coalesces motion without discarding button transitions, while all
drawing remains in process context.

## Windows

Up to six application windows can remain visible together. Clicking a window
or its Dock icon raises it, and dragging its title bar moves it with bounded
edge clamping; the Terminal viewport follows its window. Each has platinum
chrome, a title, and a real red close button. A dock click opens the window from
that application's icon using a bounded fixed-point spring: stiffness and
damping produce one controlled overshoot before exact settlement. Clicking X
or pressing Escape closes the focused window. Keyboard focus remains available
with Tab and Enter.

The spring settles in twelve 16 ms integration frames with one controlled
overshoot. Only the union of changed window and Dock rectangles is restored per
frame. Cached wallpaper rows and bounded regions avoid a full-display repaint
loop, while both animations advance from the measured timer rather than from
input-event frequency.

All graphical shell labels use a pinned Inter source rasterized to an
antialiased printable-ASCII alpha atlas at development time. The kernel uses a
small proportional SUF2 reader rather than a TrueType engine. Licensing and
the exact pinned sources are recorded in [`THIRD_PARTY_ASSETS.md`](THIRD_PARTY_ASSETS.md).

## Files

Files is a real view of the writable FAT32 data mount. Its Finder-style chrome
has compact back/up, view, action, path, and search furniture, a source-list
sidebar divided into Devices, Shared, Places, and Search For, and a sparse
folder grid. Up, the action/refresh control, the Data root, and file/folder
entries are functional. The grid is populated by `sapfs_list`, and the bottom
strip reports the actual item count and bounded free-space value. SapStudio
scratch, backup, and project bookkeeping names are hidden at the data root,
while imported media, Camera photos, and completed exports remain visible as
ordinary user files.

Creating a file or folder calls the kernel filesystem interface. Opening a
folder traverses it; opening a file hands its real path to Notes. Files never
redirects into the immutable system volume, and every status is recoverable if
the data volume is absent or corrupt.

## Notes

Notes uses a narrow note list and yellow ruled paper modeled on the supplied
classic Notes reference. Search is visual, the selected real path is shown in
both panes, the plus button creates the next available `NOTE00.TXT` through
`NOTE99.TXT`, and Ctrl-S saves. Opening a file reads its bytes from the data
volume. Printable keyboard input, Enter, and Backspace update only the bounded
in-memory buffer. Save creates the file if needed, replaces it through the
existing recovery-safe path, and synchronizes the FAT32 volume. Closing a
dirty note attempts the same bounded save.

## Settings

Settings presents a classic System Preferences-style Show All grid with twelve
honest categories: Appearance, Desktop, Dock, Displays, Keyboard, Pointer,
Performance, Network, Storage, Camera, Windows, and About. Each category shows
the bounded current Sapote configuration. Appearance is the implemented
choice: Light or Dark changes only the 3D dock shelf colour. Unsupported
hardware is reported as unavailable instead of being represented by a fake
control. Desktop shows the fourteen real wallpaper thumbnails in a compact
two-row picker and changes the cached desktop frame immediately.

## Camera

Camera presents a deliberately spare Photo Booth-style window: the live device
frame, one red camera-marked shutter, and a truthful connection status. It has
no effects, fake scenes, or background picker. Capture snapshots the provider's
latest complete frame at 320×180 as the next synchronized `PHOTO00.BMP` through
`PHOTO99.BMP`, using a temporary file and retaining no partial photo on failure.

Preview rows are assembled in a fixed scratch line and blitted to the cached
surface. New provider generations damage only the preview region.

The camera core is a real bounded double-buffered RGB888 provider: a transport
publishes complete frames, the broker protects an in-use read buffer, records
generation and drop counters, and Camera snapshots only complete frames. The
current xHCI proof still tears down after descriptor verification and does not
yet enumerate UVC streaming endpoints; the default QEMU verification machine
also exposes no camera device. Consequently Camera reports `No camera
connected` rather than substituting a wallpaper or claiming a physical feed.

## Terminal

Terminal opens on a clean dark surface with a green bitmap palette. It exposes
the existing Sapote shell and therefore retains the native filesystem commands
and the measured `linux echo`, `linux uname`, and `linux cat` profiles. `fetch`
draws the decoded red S mark itself and reports the
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
`034ba9336f6dee3cd5a524a42b740b41013ca852`. It includes the project model,
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

`make capture-redwood` boots the production ISO in QEMU with the
ordinary read-only system NVMe volume and writable data NVMe volume. QMP sends
real pointer and PS/2 keyboard input to the QEMU display. The exact 25-second
recording exercises spring dock magnification, overlapping multi-window focus,
Settings Light/Dark and photographic wallpaper selection, Files, Notes
editing/saving, SapStudio import/trim/seek/save, and restoration of the Light
dock. Camera stays closed because the QEMU proof machine has no webcam and
Redwood never fabricates a live frame. The same uninterrupted guest session
completes SapStudio export after the recording, then the capture tool retains
and inspects the data image. The retained image must contain exact-size
`SAPSTUDI.SAP` and `EXPORT.BMP` artifacts plus the enlarged Notes document, with matching FAT
copies and no cycles, cross-links, or leaked clusters. No host-rendered UI,
pasted note contents, or transcript substitution is involved.

## Retained limits

The environment has six fixed applications, six persistent movable windows,
fourteen fixed wallpaper frames at one supported geometry, printable-ASCII
text, and no arbitrary application loading. It has no physical UVC camera transport, accessibility
API, international text stack, compressed-media decoder, time-based playback,
audio output, rendered-video export, or stable graphical ABI.
The FAT32 limits and clean-sync persistence boundary remain those documented in
[`FAT32.md`](FAT32.md).

<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Sapote Redwood

Redwood is Sapote's graphical environment. It starts after the kernel has
installed the framebuffer, input, timer, storage, and desktop services.

## Desktop and Dock

The desktop offers fourteen photographic scenes at 1024×768. Sources are
converted into a compact album during the build and validated before display.
Wallpaper restoration uses cached row copies so pointer and window movement do
not repaint the complete screen.

The 3D Dock contains Files, Terminal, Notes, SapStudio, Camera, and Settings.
It uses fixed-point arithmetic for icon magnification, neighbor movement,
reflections, tooltips, press feedback, and launch bounce. Dark appearance
changes the shelf colour without changing its geometry or behavior.

## Windows

All six applications can remain open. Clicking a window raises it; dragging
the title bar moves it within the screen; the red close button closes it.
Windows open from their Dock icon with a twelve-frame spring animation. The
desktop repaints changed rectangles instead of the whole framebuffer.

Interface text uses a build-time Inter atlas. The kernel reads a small
proportional bitmap format and does not include a TrueType engine.

## Applications

### Files

Files browses the writable FAT32 data volume. Folder traversal, refresh, file
creation, folder creation, opening documents, item counts, and free-space
reporting use the kernel filesystem interface.

### Notes

Notes reads and writes text files on the data volume. It supports printable
input, Enter, Backspace, search presentation, new-note creation, and `Ctrl-S`.
Saving replaces the target through a synchronized temporary file.

### Settings

Settings provides Appearance, Desktop, Dock, Displays, Keyboard, Pointer,
Performance, Network, Storage, Camera, Windows, and About pages. Desktop and
Appearance are interactive. Hardware pages report the current configuration
and mark unavailable facilities clearly.

### Camera

Camera has a preview, connection status, and shutter. A double-buffered RGB888
provider publishes complete 320×180 frames; capture writes the next available
`PHOTO00.BMP` through `PHOTO99.BMP` to the data volume.

The standard QEMU configuration has no webcam and xHCI does not yet provide a
UVC streaming transport. Camera therefore reports `No camera connected`.

### Terminal

Terminal exposes Sapote's shell, filesystem and networking commands, and the
measured BusyBox profiles. `fetch` displays the Sapote mark and basic system
information.

### SapStudio

SapStudio provides a source browser, viewer, inspector, timeline, tracks,
clips, and a playhead. It can import an uncompressed 24-bit BMP from the data
volume, trim and save a project, and export the selected frame as a 24-bit BMP
up to 320×180.

The vendored editor foundation contains the project model, timecode, render
graph, compositing, audio, LUT, EDL, mask, transition, and freestanding-image
code. Compressed video decoding, timed playback, audio output, and rendered
video export are not yet connected to Redwood.

## Demo capture

`make capture-redwood` boots the production ISO with separate system and data
volumes. QMP sends pointer and keyboard input to the guest while the capture
opens applications, changes the Dock appearance and wallpaper, edits a note,
and uses SapStudio. Camera remains closed because the QEMU machine has no
camera source.

The same session saves the note and SapStudio project, exports a BMP, and
checks the retained data image after shutdown.

## Limits

Redwood has six fixed applications, six windows, one supported display
geometry, printable-ASCII text, and no general graphical application ABI. It
does not yet provide physical camera streaming, accessibility services,
international text shaping, compressed media, timed video playback, audio
playback, or rendered video export.

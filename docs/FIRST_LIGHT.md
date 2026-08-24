<!-- SPDX-License-Identifier: GPL-3.0-only -->

# First Light

First Light is Sapote's fixed graphical workspace. It appears only after the
framebuffer, write-combining proof, cached surface, input paths, scheduler,
layout, and closing Boot Ledger checks are installed.

It is intentionally a small kernel interface, not a general desktop or window
manager.

## Visual language

The workspace combines classic Macintosh density with a NeXT-style tool dock:
one-pixel outlines, platinum faces, bitmap type, striped title regions, compact
menus, and a slate-violet pinstriped desktop. The canonical pebble remains
unmodified and unframed.

[`BRAND.md`](BRAND.md) owns the exact logo, palette, naming, and voice. Desktop
copy uses ordinary human language; proof vocabulary such as `READY`, `ONLINE`,
and `PASS` belongs in diagnostics rather than the main interface.

## Composition

- a top menu strip with text-only branding;
- a left workspace palette;
- a right tool dock;
- a welcome window with the pebble;
- Terminal, Boot Ledger, System, and About panels;
- a keyboard focus model and PS/2 pointer cursor.

Layout is deterministic for the supported framebuffer size. Windows and dock
items have stable IDs and bounded rectangles; overlap, overflow, invalid focus,
and duplicate IDs are rejected before activation.

## Rendering and input

`surface.c` owns the cached pixel buffer, clipping, overlap-safe copies, damage
tracking, and the write-combining store fence. `ui.c` draws through that surface
and flushes bounded damage to the framebuffer. Interrupt handlers publish input
events but never draw.

The fixed event queue coalesces adjacent pointer motion while retaining button
transitions. Keyboard and pointer events are consumed in process context.
Cursor movement damages both old and new bounds so it cannot leave a trail.

The pebble source is the transparent PNG in `assets/`. A deterministic build
tool converts it to the bounded runtime stream Rust validates before C draws it.
The kernel has no PNG decoder, alpha compositor, gradient engine, or runtime
theme selection.

## Activation contract

First Light construction, activation, and installed proof are separate Boot
Ledger stages. Activation is impossible without:

- framebuffer output and its independent WC proof;
- cached surface, font, and validated layout;
- keyboard input, threads, scheduler, and closing boot proofs.

If an optional graphical prerequisite is unavailable, Sapote retains its serial
or framebuffer shell instead of publishing a partial desktop success.

## Terminal commands

The native `echo` command is unchanged. Filesystem commands operate only on the
writable data mount; `drives` reports both independently owned volumes:

```text
sap> drives
system  fat32  read-only
data    fat32  read-write
sap> mkdir projects
sap> cd projects
sap> write notes.txt "first cut"
sap> append notes.txt "second line"
sap> read notes.txt
first cut
second line
sap> sync
data synchronized
```

`mount`, `ls`, `cd`, `pwd`, `mkdir`, `touch`, `read`, `write`, `append`,
`writeat`, `truncate`, `stat`, `mv`, `rm`, `sync`, and `reboot` expose the
bounded kernel filesystem interface. Paths use the documented ASCII 8.3
subset; quoted write text receives one newline. `sync` and `reboot` establish
the clean persistence boundary. Missing or corrupt data media produces a
concise status while the prompt and immutable system commands remain usable.

The separate `linux` command exposes only three measured profiles:

```text
sap> linux echo
SAPOTE
sap> linux uname
Linux
sap> linux cat
pebble
pebble
^D
sap>
```

`help` lists the command and unsupported profile names are refused. A missing
system volume or invalid profile produces one concise error and leaves the
prompt usable. Successful output is accepted from the actual userspace `write`
or `writev` buffers; the shell contains no substitute output strings.

Only a waiting `linux cat` owns terminal input. Enter completes a line,
Backspace edits its current uncommitted line, and left Ctrl-D on an empty line
delivers EOF without passing a byte to userspace. Input is limited to four
complete lines, 64 printable ASCII bytes plus newline per line, and 256 bytes
per launch. While that foreground owner exists, input does not enter the shell
parser; teardown restores ordinary shell ownership and the prompt.

## Captures

The committed image and video come from QEMU, not a mockup:

```sh
make capture-first-light
make screenshot-proof
make capture-boot-video
```

The source captures are:

- `assets/sapote-first-light.png`;
- `assets/sapote-first-light-focus.png`;
- `assets/sapote-first-light-terminal.png`;
- `assets/sapote-first-light-boot-20s.mp4` (an approximately 20-second
  create/sync/reboot/read persistence demonstration).

`screenshot-proof` compares stable pixels and refuses a one-pixel mutation.
The `first-light` QEMU scenario checks installed state, event handling, redraw,
cursor damage, framebuffer pixels, and clean handoff. The production-path
`first-light-userland` scenario drives the same shell dispatch as an interactive
boot, launches both profiles twice, and requires prompt restoration and clean
teardown. `first-light-userland-absent` proves a missing volume is recoverable.
The `first-light-userland-interactive` scenario launches `cat` twice through
keyboard IRQ events, supplies different lines and Ctrl-D, and proves fresh
generations and prompt restoration. Its `-absent` companion omits only cat and
then runs an existing measured Linux command successfully.

Fifteen FAT32 scenarios use the same shell dispatch and NVMe path for system
loading, writable data, nested directories, multi-cluster growth, random
overwrite, truncation, rename/move, deletion/reuse, full and corrupt media,
missing data, clean-reboot persistence, cache behavior, immutable enforcement,
and handle generations.

## Limits

First Light has a fixed workspace and fixed tools. It does not provide movable
arbitrary windows, user applications, compositing, themes, accessibility APIs,
international text, or a persistent settings service.
The filesystem is deliberately bounded as described in [`FAT32.md`](FAT32.md);
it does not turn the shell into a general Unix environment.

The foreground input path is a profile-specific bounded state machine. It is
not a general stdin ABI, canonical mode, or a TTY subsystem.

# Sapote First Light

First Light is Sapote's first stateful graphical interface. It is a bounded
desktop shell in the kernel, not a boot splash, a general desktop environment,
a window manager, or a userspace boundary. The same installed framebuffer,
cached surface, screen console, shell, keyboard, interrupt topology, scheduler,
and Boot Ledger that existed before the milestone remain underneath it.

The desktop has a slate-violet pinstripe, warm platinum chrome, striped title
regions, bitmap labels, a persistent Workspace palette, one welcome window,
and a vertical tool dock. Classic Macintosh and NeXT interfaces inform the
interaction grammar; the composition and code-native icons are Sapote's own.
The pebble is decoded from the exact canonical asset and is neither recoloured
nor redrawn.

![First Light at 1024 by 768](../assets/sapote-first-light.png)

## Ownership and lifetime

`struct ui_state` is one fixed, long-lived object in `src/kernel/ui.c`. Layout,
theme, pointer position, focus, hover, press, active panel, event counters, and
render counters live together. `ui_construct` initializes it once after the
typed boot plan has established its prerequisites. `ui_activate` performs the
one initial full draw. The single long-lived shell/UI control loop then owns
all state transitions and drawing until shutdown; interrupt handlers publish
bounded input facts only.

The event queue is a fixed array of 64 `struct ui_event` entries. It has no
heap ownership and counts accepted, drained, coalesced, and dropped events.
Adjacent pointer movements coalesce. When a non-movement event reaches a full
queue, the oldest movement is evicted if one exists. A full queue containing
only transitions refuses the new event and increments `dropped`; it never
overwrites unreported state. Idle control flow executes `hlt` and does not
redraw or busy-spin.

Public types are in `include/sapote/ui.h`: named status, event, element,
panel, action, and pointer-button enums plus `ui_point`, half-open `ui_rect`,
`ui_event`, `ui_theme`, `ui_layout`, `ui_dock_item`, `ui_state`, and
`ui_proof`. The cursor is a 12 by 18 code-native mask with hotspot `(0, 0)`.

## Theme and composition

Only the documented identity palette is installed:

| Role | RGB |
| --- | --- |
| Warm white | `#F7F6F0` |
| Ink and outlines | `#101012` |
| Desktop | `#595976` |
| Desktop rule | `#666684` |
| Active selection | `#18181C` |
| Inactive title rule | `#7A7A82` |
| Teal accent | `#4F837F` |
| Gold accent | `#C4A44E` |
| Green accent | `#598561` |
| Red accent | `#A55050` |
| Violet accent | `#705984` |
| Shadow | `#353542` |
| Window face | `#D7D6CE` |

The accents belong to tool icons, never to readiness lights or the canonical
mark. There is no floating point, alpha compositor, kernel-generated gradient,
transparency, animation, or runtime theme.

The normal desktop contains a 24-pixel type-only menu bar, a 132 by 202
Workspace palette, a responsive welcome window up to 640 by 388, the canonical
mark at its deterministic 280 by 258 runtime size, a short welcome message, a
version label, and a four-item vertical tool dock. Diagnostic state stays in
the Ledger and System tools. No default-desktop label uses readiness language,
an address, a cycle count, or a timing value.

## Deterministic layout

`ui_layout_build` accepts widths from 800 through 1920 and heights from 600
through 1200. Smaller or larger modes return `unsupported First Light
framebuffer geometry`. Construction is pure and validation happens before the
first desktop draw.

The constants are:

- menu bar: full width by 24 pixels;
- Workspace palette: 132 by 202 at `(8, 38)` with one inverted heading and
  eight fixed rows;
- welcome window: the lesser of 640 pixels and the space between the palette
  and dock, by 388 pixels, at y=48 for 600-pixel modes and y=60 otherwise;
- mark: 280 by 258 in ordinary modes; 210 by 194 at the minimum 800-pixel width;
- content column: 246 pixels wide, forty pixels from the welcome window's right
  edge;
- version label: 246 by 20 in the content column;
- tool dock: 84 by 274, eight pixels from the right and fourteen below the menu;
- tool items: four 70 by 62 half-open rectangles, four pixels apart and seven
  pixels inside the dock;
- icons: 28 by 28, centred above each item label;
- panel width: the lesser of 720 and `surface width - 156`;
- panel height: 400 at heights of 720 or more, otherwise 330;
- panel position: centred in the area to the left of the tool dock;
- panel client: eight pixels inside each side, beginning 36 pixels below the
  panel top.

At 1024 by 768 the welcome window begins at `(224,60)`, the mark at `(244,118)`,
the tool dock at `(932,38)`, and the panel at `(110,184)`. At 800 by 600 those
origins are `(164,48)`, `(184,106)`, `(708,38)`, and `(36,135)`. At 1280 by
720 they are `(352,60)`, `(372,118)`, `(1188,38)`, and `(238,160)`.

Validation proves every rectangle is inside the surface, the four items do not
overlap, the client is non-empty, each baseline fits, the cursor hotspot is
inside its mask, and each interactive item owns one unique typed ID. All
rectangles are half-open: left and top edges belong to a rectangle; right and
bottom edges do not. Hit testing scans every item and refuses ambiguity rather
than making declaration order a z-order rule. Checked addition is used before
rectangle ends, intersections, unions, and text advances are accepted.

## Dock and panels

The first four items are fixed and typed:

| Item | Element ID | Action | Panel |
| --- | --- | --- | --- |
| Terminal | `UI_ELEMENT_DOCK_TERMINAL` | `UI_ACTION_TOGGLE_TERMINAL` | `UI_PANEL_TERMINAL` |
| Ledger | `UI_ELEMENT_DOCK_LEDGER` | `UI_ACTION_TOGGLE_LEDGER` | `UI_PANEL_LEDGER` |
| System | `UI_ELEMENT_DOCK_SYSTEM` | `UI_ACTION_TOGGLE_SYSTEM` | `UI_PANEL_SYSTEM` |
| About | `UI_ELEMENT_DOCK_ABOUT` | `UI_ACTION_TOGGLE_ABOUT` | `UI_PANEL_ABOUT` |

Each has normal, hovered, focused, pressed, and active states. Focus is a
one-pixel dotted gold inset, so it remains visible without a pointer. The
Terminal, Ledger, System, and About icons are drawn from bounded fill and stroke
primitives; there is no icon font, emoji, image, callback payload, or launch
animation.

Only one fixed panel can be open. `Tab` advances focus, `Shift+Tab` moves it
backward, `Enter` activates, and `Escape` closes. Focus wraps across all four
items. Terminal installs the existing screen console into the validated panel
client and uses the existing shell parser. Ledger reads the published receipt
set. System reports stable CPU, memory, PCI, timer, and framebuffer shapes with
no physical addresses. About identifies Sapote 0.9.0 and the First Light /
Pebble milestone. Panels are neither draggable nor resizable.

Shell characters are routed to the graphical terminal only while Terminal is
active. Serial output remains independent. The screen retains a fixed 160 by
48 cell backing store and reflows the most recent rows into a new validated
viewport, so hiding and reopening Terminal preserves a bounded tail. A clipped
terminal redraw cannot reach the dock or cursor.

## Rendering and cursor

All desktop work goes through `struct surface`; UI and pointer code contain no
direct framebuffer write. The initial activation may present the full surface.
Later cursor, focus, dock, and panel transitions union bounded damage and
present only that rectangle. The surface's existing volatile WC row copy and
post-store `sfence` are unchanged and are statically required by `make verify`.
The welcome window, Workspace palette, tool dock, and panel drop shadows have
checked bounds. A panel
transition damages the union of the window and its six-pixel shadow, so closing
or replacing a panel cannot leave pixels outside the declared window rectangle.

The cursor mask is drawn last in ink and white. Movement unions its old and
new 12 by 18 bounds, restores the cached pixels below the old mask, then draws
at the new clamped position. The IRQ12 handler never draws. When the pointer is
declared absent, `pointer_present` is false and no mask is composed.

Render counters cover full draws, damaged draws, pixels copied, cursor moves,
dock-state changes, panel transitions, damage rectangles, and glyphs. Drawing
the same installed state twice must reproduce the complete cached-surface hash;
a one-state mutation must change the synthetic hash.

## Boot Ledger transition

First Light adds these stages:

1. `BOOT_STAGE_UI_FONT`
2. `BOOT_STAGE_POINTER_DECISION`
3. `BOOT_STAGE_POINTER_OUTCOME`
4. `BOOT_STAGE_UI_LAYOUT`
5. `BOOT_STAGE_DESKTOP_CONSTRUCTION`
6. `BOOT_STAGE_DESKTOP_ACTIVATION`
7. `BOOT_STAGE_FIRST_LIGHT_PROOF`

It adds capabilities for verified UI font, pointer decision, mutually
exclusive pointer-present and pointer-absent outcomes, validated layout,
available and activated desktop shell, and completed installed proof. Pointer
absence is a neutral optional skip: it contributes the absence capability and
does not degrade the ledger.

Desktop activation has ten declared prerequisites: desktop construction,
installed framebuffer output, the independent framebuffer-WC proof, cached
surface, verified UI font, validated layout, keyboard, threading, scheduler,
and inherited closing boot proofs. Installed verification also asserts the WC
and scheduler edges semantically, so deleting either requirement after plan
validation is rejected by the named capability.

The bounded capacities are 40 stages, 40 receipts, fourteen capabilities per
stage, and two proof counters per receipt. A normal QEMU boot records all 40
receipts; absent device-substrate, xHCI, NVMe and filesystem fixtures each
contribute one neutral skip.
Fingerprints remain build-plan dependent. Pointer absence contributes its own
neutral skip without degrading the installed ledger.

Framebuffer, font, layout, construction, or activation failure is an optional
degraded path. It cannot forge later capabilities, and the already-established
serial shell remains reachable.

## Installed proof and QEMU scenario

The installed proof checks the canonical logo through the unchanged asset
pipeline; the UI font receipt, bytes, fingerprint, and metrics; exact theme;
validated installed layout; all four item IDs/actions once; valid focus,
hover, press, and panel IDs; cursor bounds or declared absence; WC-before-draw
ordering; scheduler, thread, memory, keyboard, and closing-proof dependencies;
single execution through receipts; and exact plan and receipt fingerprints.
It then redraws the same state and requires an identical complete surface hash.

`make qemu-test-first-light` uses guest exit `0x2F` and host status 95. It walks
the real installed ledger, injects real three-byte packets through 8042 command
`0xD3`, exercises hover/press/release and every panel, runs keyboard navigation,
checks the synthetic absence plan, proves old/new cursor damage, reads selected
logo/text/dock/panel/cursor pixels, and recomputes the ledger fingerprint. The
workbench renders the decoded logo once as a two-color, one-pixel bitmap
directly on the warm window face and applies ordered dithering only to its
antialiased edge shades. The menu bar uses type instead of a miniature logo, so
the mark has no separate tile, field, frame, or secondary status strip.

`tools/capture-first-light.py` uses QMP to capture the emulated display itself.
It produces clean, focus/hover, and terminal-with-ledger frames. The committed
PNG files are 1024 by 768 and are not edited. The comparator treats the full
frame as stable because these views expose no variable timings or addresses;
its self-test flips one stable pixel and requires refusal.

## Deliberate controls

Every mutation below used a file snapshot, a clean build, the narrowest proof,
and snapshot restoration. The observed results are from QEMU TCG unless the
control is explicitly a source/comparator assertion.

| # | Deliberate break | Observed refusal |
| ---: | --- | --- |
| 1 | Corrupt packed-font magic. | Pure boot stage: `UI font header is missing or malformed`. |
| 2 | Remove the last declared glyph byte. | Pure boot stage: `UI font bitmap is truncated`. |
| 3 | Duplicate the Ledger item ID. | Pure boot stage: `duplicate UI element identifier`. |
| 4 | Place dock items one pixel into each other. | Pure boot stage: `dock item rectangles overlap`. |
| 5 | Wrap the mark rectangle's right edge. | Pure boot stage: `UI rectangle arithmetic overflowed`. |
| 6 | Remove activation's WC prerequisite. | Installed ledger: `stage desktop activation; capability framebuffer WC independently proved`. |
| 7 | Remove activation's scheduler prerequisite. | Installed ledger: `stage desktop activation; capability scheduler available`. |
| 8 | Call `ui_activate` from `kernel.c`. | Build assertion: `First Light boot stage bypasses the Boot Ledger`. |
| 9 | Call `ui_flush` in the IRQ12 handler. | Build assertion: `PS/2 pointer interrupt path attempts UI drawing`. |
| 10 | Damage only the new cursor bounds. | `ST FAIL first-light: First Light cursor damage left a trail`. |
| 11 | Remove the cached-surface fence call. | Build assertion: `cached-surface WC present lost its sfence`. |
| 12 | Write one UI pixel through `framebuffer_write_pixel`. | Build assertion: `First Light bypasses the cached surface`. |
| 13 | Disable adjacent movement coalescing. | Pure boot stage: `pointer movement did not coalesce in the fixed event queue`. The valid test also fills 64 entries and retains the button transition by evicting movement. |
| 14 | Accept an unsynchronised first packet byte. | Pure boot stage: `PS/2 packet desynchronization did not recover`; the valid continuation produces no phantom button. |
| 15 | Force declared pointer absence. | `boot-ledger` passed with 30 stages, 30 receipts, 34 capabilities, one neutral skip, no pointer-success capability, and active keyboard focus. |
| 16 | Force desktop construction failure. | Interactive boot retained `sap>`; activation/proof lines and their capabilities were absent. |
| 17 | Falsify the UI-font receipt size. | Installed ledger: `stage First Light UI font; capability UI font verified`. |
| 18 | Change the UI-font stage ID after validation. | `plan fingerprint mismatch`. |
| 19 | Delete the permanent installed-proof line. | Normal boot exited normally, then the transcript comparator refused: `normal transcript omitted permanent First Light proof line`. |
| 20 | Flip one stable screenshot pixel. | Comparator: `single stable-pixel mutation refused`. |
| 21 | Change the First Light guest exit to `0x30`. | Exit comparator observed host 97 instead of required 95. |
| 22 | Limit panel-transition damage to the window while retaining its six-pixel shadow. | The final installed redraw proof refused the stale shadow: `First Light final installed redraw proof failed`. |

No correctness control passed unexpectedly.

## Verification contract

The current repository has 39 named QEMU scenarios. Pull requests run
`make verify`, capture all three First Light frames from QEMU, compare every
stable pixel, and execute all 39 scenarios. The milestone evidence workflow
then runs ten complete serial TCG sweeps and records the authorized accelerator
surface. It does not probe KVM through the excluded `/dev/kvm` host device.
Execution results and flakes belong in the workflow artifacts and release
verification summary rather than being copied into this interface contract.

The dedicated `first-light` scenario must exit through guest value `0x2F` and
host status 95, with exactly one `ST BEGIN first-light` and one
`ST PASS first-light`. The committed clean, focus/hover, and terminal/ledger
images must match fresh QEMU captures, and each comparator mutation self-test
must refuse one changed stable pixel. `make verify` separately requires the
canonical logo hash, byte-reproducible packed assets, warning-free link,
resolved symbols, non-RWX load segments, no linked relocations, no
floating-point/MMX/SSE/AVX kernel text, no direct UI framebuffer writes, and no
drawing from IRQ12.

With FFmpeg installed, `make capture-boot-video` regenerates the committed
20-second H.264 clip from QEMU's emulated display. The recorder starts at
firmware, requires the installed Boot Ledger proof, opens Terminal, and enters
`version`; it does not recreate the interface on the host.

## Limits

First Light itself is a kernel-owned workspace and does not yet host userspace
applications. It has no networking, GPU acceleration, USB/Bluetooth/touch input, wheel,
gestures, live mode switching, animation, dynamic fonts/themes/modules, or SMP
scheduler expansion. It is not a compositor security boundary or a formally
verified GUI. Window management, movable/resizable windows, process isolation,
and physical-machine display/input evidence remain later work.

# PS/2 pointer input

First Light adds one optional PS/2 auxiliary-device path. It is deliberately
small enough for QEMU and compatible 8042 hardware: three-byte packets, three
buttons, relative movement, and no wheel, acceleration, USB, touch, gesture,
or theme protocol.

## Sharing the 8042

The keyboard owns the existing controller bring-up. Pointer discovery requires
that keyboard initialization and the discovered I/O APIC already be complete,
and it refuses to run while interrupts are enabled. It enables and tests only
the second port; it does not reset the controller or reinitialize the keyboard.

All input-buffer and output-buffer waits are bounded by 100,000 polls. Named
statuses distinguish controller timeout, a stuck auxiliary clock, port-test
failure, device refusal, handler registration failure, I/O APIC routing
failure, invalid bounds, absence, and injection failure.

The device receives Set Defaults (`0xF6`) and Enable Reporting (`0xF4`), each
requiring ACK `0xFA`. The driver installs vector 60, routes ISA IRQ12 through
the already-discovered I/O APIC, then enables the second-port interrupt bit.
Any failure masks/unregisters what was installed and records a valid absent
decision.

Availability uses two boot stages because decision and outcome have different
semantics. Decision always establishes
`POINTER_AVAILABILITY_DECIDED`. Outcome either runs and establishes
`POINTER_INPUT_AVAILABLE`, or makes a neutral optional skip and establishes
`POINTER_INPUT_ABSENT`. Installed verification rejects both capabilities
together, neither capability, or a hardware state inconsistent with the
receipt. Absence does not degrade the Boot Ledger and leaves the desktop on its
keyboard focus path.

## Packet decoder

`struct pointer_state` is fixed and long-lived. It holds decision/presence,
three button bits, bounded coordinates, the three-byte packet cursor, and
counters for interrupts, bytes, packets, movements, button transitions,
overflows, and desynchronizations.

Bit 3 must be set on the first byte. A byte without it while the decoder is at
packet index zero is discarded and increments `desynchronizations`; the next
valid first byte restarts the packet without a phantom click. X/Y sign is
decoded through `int8_t`. Device Y is inverted into screen Y. Either overflow
bit discards the packet's movement and button interpretation and increments
`overflows`.

Coordinate arithmetic promotes the current unsigned coordinate and signed
delta to `int64_t`, then clamps to `[0,width-1]` and `[0,height-1]`. Bounds must
be nonzero and no greater than `INT32_MAX`, because UI event points are signed
32-bit values. The pure decoder test covers positive and negative deltas, both
overflow bits, desynchronization/recovery, press/release without a phantom
state, and both top-left and bottom-right clamps.

## IRQ and event handoff

The IRQ12 handler reads at most 64 auxiliary bytes per entry. It updates the
bounded decoder and publishes `ui_event` facts. It allocates nothing, calls no
surface or framebuffer function, and never processes or draws UI state.
`make verify` rejects UI processing, flush, or surface presentation calls in
`pointer.c`.

The shared 64-entry UI queue coalesces adjacent movements. Press and release
are distinct events for left, middle, and right buttons. A non-movement event
arriving at a full queue evicts the oldest movement when possible; if the queue
contains only transitions, the new event is refused and `dropped` increments.
Accepted, drained, coalesced, and dropped counts remain visible in `ui_state`.

The installed QEMU proof uses real controller command `0xD3` to put each test
byte into the auxiliary output path. IRQ12, vector dispatch, packet decode,
event publication, UI draining, and damage therefore run normally; only the
physical hand movement is injected.

## Cursor composition

The cursor is a code-native 12 by 18 outer/inner bit mask with hotspot `(0,0)`.
It is drawn last through the cached surface, black at the outline and white
inside. Movement damages the union of old and new bounds so cached content is
restored before the mask is composed again. Edge clipping may reduce the
visible rectangle but never changes the hotspot. Pointer absence skips cursor
composition entirely.

The First Light scenario moves the real software cursor through the event
path, checks hover/press/release, verifies the old hotspot is restored to its
underlying desktop pixel, and verifies the new hotspot contains the cursor.
Removing old-bound damage fails with `First Light cursor damage left a trail`.

## Limits

There is no wheel, IntelliMouse extension, acceleration curve, USB HID,
hotplug, suspend/resume, touchpad gesture, or custom cursor. Controller sharing
is single-processor and uses the existing interrupt-masked queue handoff. Real
hardware beyond compatible 8042 implementations remains an evidence gap.

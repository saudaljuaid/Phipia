# I/O APIC interrupt routing

This increment delivers a real interrupt through hardware Seneri discovered
rather than hardware the PC inherited. The timer now reaches the processor by
two independent paths, and both are proved on every boot.

Level-triggered routing was deferred when that landed, and this document now
covers it too: the second acknowledgement it needs, the register that carries
it, and the machines on which Seneri refuses to attempt it.

## Programming invariants

Each discovered I/O APIC is reached through an index register and a data window
sixteen bytes above it, not as a flat register file. A select followed by a
window access is one indivisible transaction, so every caller runs with
interrupts disabled, and both registers are `volatile` because they are device
state whose accesses must reach the device exactly once in program order.

The directed end-of-interrupt register is the exception to that shape. It sits
at memory offset 0x40 and is a register in its own right rather than an index
the window pair selects, so writing it is a single access and needs no
transaction around it.

Firmware is held to its own description, as with the local APIC. The identifier
in each I/O APIC's ID register must match what the MADT declared. A unit must
be able to redirect at least the sixteen ISA interrupts. Two I/O APICs may not
claim overlapping global system interrupts, because routing would then depend on
which one Seneri happened to examine first.

Initialization masks every redirection entry on every discovered unit. Firmware
may leave entries unmasked, and none of them are Seneri's until Seneri programs
them.

Every routed vector is recorded against the unit and pin it landed on, because
the dispatcher has only the vector when an interrupt arrives and a directed end
of interrupt has to reach the unit that owns the entry. Re-routing a pin forgets
every vector that previously named it: a stale record would let an end of
interrupt for a vector nothing delivers reach a live entry. Pointing a vector at
a second pin is refused outright, because the entry it named before would stay
unmasked and keep delivering a vector the dispatcher would then acknowledge
somewhere else. The delivery window
holds exactly one vector per ISA interrupt, and a `_Static_assert` ties the two
bounds together, so a vector that passes the range check always has a record and
the table cannot be overrun.

Only the fields Seneri wrote are compared when an entry is read back. Delivery
status at bit 12 and remote IRR at bit 14 belong to the device and either can be
set the instant an unmasked pin asserts, so comparing the raw word would fail a
correctly programmed level-triggered entry whenever its line happened to be
busy. A second `_Static_assert` keeps those two bits out of the writable mask.

## Routing a legacy interrupt

An ISA IRQ is not its own global system interrupt. ACPI's interrupt source
overrides say where it actually lands and how it is wired, and Seneri applies
them: the override's global system interrupt selects the redirection entry, and
its MPS INTI flags select polarity and trigger mode. Without an override, ACPI
section 5.2.12.5 makes an ISA IRQ identity mapped, edge triggered, and active
high, which is what Seneri assumes and nothing more.

Both flag fields reserve one encoding. A reserved encoding describes electricals
Seneri cannot program, so it is refused by name rather than read as the nearest
thing that fits — a reserved polarity is not "active high because the bit that
means active low is clear".

On the supported target this is not academic. Firmware overrides IRQ0 to global
system interrupt 2, so a router that ignored the override would program a
correct-looking entry for the wrong pin and receive nothing. The same firmware
declares IRQ 5, 9, 10 and 11 level triggered and active high; those are the PCI
link interrupts, and until this increment Seneri refused to route them at all.

A redirection entry is written masked first, then its destination, then its
unmasked vector, so no interrupt can be delivered against a half-written entry.
The entry is read back afterwards, comparing only the fields Seneri wrote.

## Level-triggered routing

An edge-triggered pin needs no acknowledgement at the I/O APIC. It is sampled on
a transition, nothing is latched, and the local APIC's end of interrupt is the
whole of the protocol.

A level-triggered pin latches remote IRR when it delivers, and while that bit is
set the pin will not deliver again. Intel SDM volume 3A section 11.5.5 and the
I/O APIC datasheet give exactly one way to clear it from software: write the
vector to the I/O APIC's own end-of-interrupt register. That register arrived
with I/O APIC version 0x20. On anything older there is nothing to write, and a
level-triggered entry programmed on such a unit would deliver one interrupt and
then wedge forever.

Seneri therefore reads each unit's version register, records whether it has the
directed end-of-interrupt register, and refuses a level-triggered entry on a
unit that does not — the same shape as `paging.c` asking
CPUID.80000001H:EDX[20] before promising no-execute. The refusal is named
`IOAPIC_STATUS_NO_DIRECTED_EOI`, and normal boot panics on such a machine rather
than continuing without the routing it just claimed.

**The mask and unmask fallback is deliberately not implemented.** An older I/O
APIC can be coaxed into clearing remote IRR by masking the entry, rewriting it
edge triggered, and restoring it — the sequence Linux uses on pre-0x20 parts.
Seneri does not, for one reason: the supported target reports version 0x20, so
the fallback could not be exercised by any test in this repository. An
acknowledgement path that has never run is worse than a refusal that has, and
this kernel does not carry code no test can reach. Refusing is the documented
answer until there is a machine to prove the fallback on.

## Acknowledgement

Vectors 48 through 63 are reserved for I/O APIC delivery and are deliberately
disjoint from the 8259 range at 32 through 47. The dispatcher can therefore tell
from the vector alone which controller must be acknowledged: an I/O APIC
interrupt ends at the local APIC's EOI register, a legacy interrupt at the 8259
pair.

A level-triggered vector ends at both, and the order is fixed:

1. The handler runs, and quiets the device.
2. The I/O APIC is acknowledged, clearing remote IRR.
3. The local APIC is acknowledged, clearing the vector from service.

Each step is load-bearing and each is proved by a control below.

The first is the device's, not the interrupt controller's, and it must come
first. A pin acknowledged while its source is still asserting is re-serviced
inside the acknowledgement itself, so the interrupt arrives again immediately
and the machine makes no progress. This is the discipline every level-triggered
driver owes its hardware, and Seneri's one device is no exception: on this route
the 8254 runs in mode 0, whose output goes high at the terminal count and stays
high until a new count is written, so the handler writes one.

The I/O APIC goes before the local APIC because while the local APIC still holds
the vector in service the processor cannot accept it again. A re-assertion that
the directed end of interrupt releases is therefore queued rather than taken,
and the local APIC's EOI stays the single instant at which the interrupt is
finished. Both acknowledgements follow the handler, so a second interrupt from
the same source cannot arrive while the first is still running.

The entry is deliberately *not* read back after the acknowledgement. A line that
is still asserted is re-serviced inside that write and latches remote IRR again,
so a set bit afterwards means the next delivery has already happened rather than
that this one failed. There is no state such a read could distinguish, so the
check is not written. What is checked is the bit *before* the write: a
level-triggered delivery must have latched remote IRR, and a count of the times
it had not is kept and required to be zero.

## Two routes, one timer

`pit_start` takes the route as an argument rather than assuming one. The legacy
path unmasks IRQ0 at the 8259; the I/O APIC path leaves every legacy line masked
and programs a redirection entry instead; the level path programs the same entry
level triggered and puts the 8254 in mode 0 so its line holds. Exactly one
controller can deliver IRQ0 at a time, so a duplicated tick would be a routing
bug rather than a race.

Firmware declares IRQ0 edge triggered, and Seneri's production routing honours
that. Trigger mode is a property of how the I/O APIC samples a pin rather than
of the pin itself, so the level route asks for it explicitly through
`IOAPIC_TRIGGER_FORCE_LEVEL`. That override exists because the 8254 is the only
interrupt source this kernel owns: without it the level path could not be
executed at all until there is a PCI driver, and an untested acknowledgement
path is exactly what the previous paragraph refuses to ship. Nothing but the
proof uses it.

Normal boot proves all three, in order. Keeping the legacy proof alive is what
makes this increment reversible: if I/O APIC delivery regresses, the 8259 path
is still known good on the same boot.

## Bounded waiting

A route that stops delivering must fail, not hang. `pit_wait_for_ticks` halts
until the next interrupt, which is the cheapest way to wait and the wrong way to
prove that a route works: the failure this increment exists to catch is a line
that goes quiet, and halting for an interrupt that is never coming produces a
timeout rather than a reason.

`pit_wait_for_ticks_bounded` spins on the ACPI power management timer instead
and reports how long the wait took. A line that stops is
`PIT_STATUS_DELIVERY_TIMEOUT`; a line that delivers far too fast shows up in the
measured interval. Its bound is refused above two seconds, because the reference
counter is 24 bits at 3.579545 MHz and a span may only fold a single wrap.
Normal boot's route proofs use it, so a broken acknowledgement is diagnosed on
every boot rather than waited out.

## Executable proof

`ioapic_self_test` runs before any hardware is touched and is driven entirely by
synthetic register values:

- the override resolver — an unoverridden IRQ stays identity mapped, edge
  triggered and active high; the customary IRQ0 to GSI2 override is followed;
  all four corners of the polarity and trigger encoding decode correctly; and an
  unoverridden IRQ keeps its mapping beside overridden ones;
- both reserved encodings are refused by name, and every rejection leaves its
  output zeroed rather than half filled;
- redirection entries round-trip through composition and decomposition for both
  trigger modes, both polarities and both mask states; composition never places
  a bit the I/O APIC owns; and remote IRR decodes when the device sets it;
- the version check answers no below 0x20 and yes from 0x20 up;
- every routing rejection: an IRQ outside the ISA range, a vector on either side
  of the delivery window, a destination too wide for the entry field, and an
  unknown trigger request;
- the acknowledgement's target resolution, including that a record is forgotten
  exactly once so the level-route count cannot run away;
- and every public entry point refusing by name before initialization.

The `ioapic-level` QEMU scenario proves the hardware path. It requires the
discovered unit to have the directed end-of-interrupt register, refuses to
acknowledge an unrouted or out-of-range vector, routes the timer level
triggered, reads the entry back off the hardware to confirm it really is level
triggered and unmasked on the pin ACPI named, refuses to point that vector at a
second pin while it is live, and then counts eight deliveries.

Eight, because one proves nothing: a pin whose remote IRR is never cleared
delivers exactly once, and one delivery is what success and that failure have in
common. The scenario also holds the eight to the interval eight ticks of a
100 Hz timer take, and refuses more than sixteen of them, because the opposite
failure is a pin acknowledged too early that re-delivers as fast as the
processor will accept it. It then re-routes the same pin edge triggered and
requires both counters to stand still across eight more deliveries, so an
implementation that treated every route as level triggered would not pass.

The `ioapic` scenario is unchanged and still proves the edge path.

Normal boot additionally requires:

```text
Seneri OS: I/O APIC id 0 version 0x0000000000000020 entries 24 base GSI 0 directed EOI yes
Seneri OS: I/O APIC level route id 0 GSI 2 vector 48 active high
Seneri OS: I/O APIC level deliveries 8 remote IRR 8 directed EOI 8 in 79892556 ns
Seneri OS: I/O APIC delivered eight level-triggered interrupts
Seneri OS: level-triggered routing established
```

## Negative controls

Each was applied to a clean tree, built, run, and reverted.

| Control | Observed |
| --- | --- |
| The local APIC end of interrupt is dropped, the directed one kept | `ST FAIL ioapic-level: the level-triggered line stopped delivering`, and `PANIC: PIT route stopped delivering before its deadline` on normal boot |
| The two acknowledgements are swapped | `ST FAIL ioapic-level: a level-triggered delivery did not latch remote IRR`, with remote IRR observed 0 times of 8 |
| The I/O APIC is acknowledged before the handler quiets the source | the same failure, with eight deliveries in 40 ms instead of 81 ms and remote IRR observed 4 times of 8 |
| The handler never quiets the source | `ST FAIL ioapic-level: a level-triggered line delivered without stopping` — 5001 deliveries in 23 ms |
| The entry is programmed edge triggered while the route record claims level | `ST FAIL ioapic-level: the timer would not take the level-triggered route`, from the read-back at routing time |
| The same, with the routing read-back also blinded to trigger mode | `ST FAIL ioapic-level: the level route did not read back level triggered`, from the scenario's own read of the hardware |
| The machine's I/O APIC reports version 0x11 (`-global ioapic.version=0x11`, no source change) | `ST FAIL ioapic-level: I/O APIC predates the directed end-of-interrupt register`, and the same text as a panic on normal boot |
| **The directed end of interrupt is never written** | **no failure — recorded as a non-result** |

The last one is the interesting one, and dressing it up would be worse than
reporting it. With the write to register 0x40 suppressed and everything else
intact, both normal boot and the scenario still pass: eight deliveries, remote
IRR observed on all eight. Something else is clearing it.

That something is the local APIC. On this target the local APIC broadcasts its
end of interrupt to the I/O APICs for any vector delivered level triggered, and
that broadcast clears remote IRR just as the directed write does. The swapped
control above is the evidence: with the local APIC acknowledged first, Seneri's
own read finds remote IRR already clear on every delivery.

Intel SDM volume 3A section 11.8.5 provides for turning that broadcast off — bit
12 of the spurious interrupt vector register, EOI-broadcast suppression — which
would leave the directed write as the only thing that could clear the bit. This
target does not offer it. Bit 24 of the local APIC version register, which
reports support, is clear, so Seneri does not set bit 12; and writing it anyway
as an experiment showed why the report is honest: the register reads back
`0x1FF`, the write having been truncated to nine bits. The suppression cannot be
enabled on this machine, so the control cannot be made to fail on it.

What that leaves is a directed end of interrupt whose necessity is argued from
the specification rather than demonstrated on this target, and which is
demonstrably harmless and correctly ordered here. It is the first acknowledgement
sent, so on the machine where it is the only one that works, it is the one doing
the work; and the counters above prove that remote IRR is genuinely latching on
every delivery, which is the state it exists to clear. That is the honest extent
of the claim.

## Deferred work

- **The mask and unmask fallback for I/O APICs older than version 0x20.**
  Refused rather than implemented, for the reason above. It needs a machine that
  reports an older version *and* can deliver a level-triggered interrupt to be
  worth writing.
- **EOI-broadcast suppression.** Seneri does not set bit 12 of the spurious
  interrupt vector register, because the supported target does not report
  support for it. Setting it where it is supported would make the directed end
  of interrupt the only acknowledgement of a level-triggered pin, and would turn
  the non-result above into a control that fails.
- **A real level-triggered device.** The 8254 in mode 0 is a faithful model of
  one — it holds its line until software puts it down — but it is a model. Every
  PCI device shares a level-triggered line, and none of them can be reached
  until there is PCI enumeration.
- **MSI and MSI-X.** They remove the I/O APIC from the path entirely, which is a
  separate increment and not a substitute for this one: a message-signalled
  interrupt is edge-like by construction, and the pin-based path still carries
  every device that predates it.
- **The older scenarios still wait unbounded.** `apic`, `ioapic`, `retired`,
  `pit` and the clock scenarios halt for their interrupts, so a regression in
  their delivery is a harness timeout rather than a diagnosis. Normal boot's
  route proofs are bounded; converting the scenarios is a change of its own.
- **Nothing here is per-processor.** Every redirection entry targets the
  bootstrap processor by physical identifier, and the route table is a single
  static array. A second processor needs a policy for which one a pin should
  interrupt before it needs any code.
- **Verified under QEMU only**, plus one run with the I/O APIC's version forced
  to 0x11. The remote IRR behaviour in particular is the kind of thing an
  emulator can model more forgivingly than silicon does.

# I/O APIC interrupt routing

This increment delivers a real interrupt through hardware Seneri discovered
rather than hardware the PC inherited. The timer now reaches the processor by
two independent paths, and both are proved on every boot.

## Programming invariants

Each discovered I/O APIC is reached through an index register and a data window
sixteen bytes above it, not as a flat register file. A select followed by a
window access is one indivisible transaction, so every caller runs with
interrupts disabled, and both registers are `volatile` because they are device
state whose accesses must reach the device exactly once in program order.

Firmware is held to its own description, as with the local APIC. The identifier
in each I/O APIC's ID register must match what the MADT declared. A unit must
be able to redirect at least the sixteen ISA interrupts. Two I/O APICs may not
claim overlapping global system interrupts, because routing would then depend on
which one Seneri happened to examine first.

Initialization masks every redirection entry on every discovered unit. Firmware
may leave entries unmasked, and none of them are Seneri's until Seneri programs
them.

## Routing a legacy interrupt

An ISA IRQ is not its own global system interrupt. ACPI's interrupt source
overrides say where it actually lands and how it is wired, and Seneri applies
them: the override's global system interrupt selects the redirection entry, and
its MPS INTI flags select polarity and trigger mode. Without an override, ACPI
section 5.2.12.5 makes an ISA IRQ identity mapped, edge triggered, and active
high, which is what Seneri assumes and nothing more.

On the supported target this is not academic. Firmware overrides IRQ0 to global
system interrupt 2, so a router that ignored the override would program a
correct-looking entry for the wrong pin and receive nothing.

A redirection entry is written masked first, then its destination, then its
unmasked vector, so no interrupt can be delivered against a half-written entry.
The entry is read back afterwards, comparing only the fields Seneri wrote.

Level-triggered sources are refused rather than routed. Each delivery leaves the
remote IRR set until the entry is acknowledged at the I/O APIC as well, which
this increment does not implement, so routing one would deliver a single
interrupt and then wedge. Refusing names the limit instead of hiding it.

## Acknowledgement

Vectors 48 through 63 are reserved for I/O APIC delivery and are deliberately
disjoint from the 8259 range at 32 through 47. The dispatcher can therefore tell
from the vector alone which controller must be acknowledged: an I/O APIC
interrupt ends at the local APIC's EOI register, a legacy interrupt at the 8259
pair. Both acknowledge after the handler returns, so a second interrupt from the
same source cannot arrive while the first is still running.

## Two routes, one timer

`pit_start` now takes the route as an argument rather than assuming one. The
legacy path unmasks IRQ0 at the 8259; the I/O APIC path leaves every legacy line
masked and programs a redirection entry instead. Exactly one controller can
deliver IRQ0 at a time, so a duplicated tick would be a routing bug rather than
a race.

Normal boot proves both, in order. Keeping the legacy proof alive is what makes
this increment reversible: if I/O APIC delivery regresses, the 8259 path is
still known good on the same boot.

## Executable proof

`ioapic_self_test` proves the override resolver without hardware, since that is
the part that decides where an interrupt actually goes: an unoverridden IRQ stays
identity mapped, edge triggered and active high; the customary IRQ0 to GSI2
override is followed; conforming, level-triggered and active-low flag encodings
are decoded correctly; an unoverridden IRQ keeps its mapping beside overridden
ones; and null arguments, out-of-range IRQs and out-of-range vectors are refused.

The `ioapic` QEMU scenario proves the hardware path: the I/O APIC is
initialized, every legacy PIC line is masked before and after routing, the timer
takes the I/O APIC route, eight interrupts arrive, and no spurious interrupt is
raised.

Normal boot additionally requires:

```text
Seneri OS: I/O APIC online
Seneri OS: I/O APIC delivered eight interrupts
```

Two negative controls confirm the scenario is not vacuous. Ignoring the ACPI
override, so IRQ0 routes to global system interrupt 0 instead of 2, stops
delivery and hangs the guest until the scenario times out. Dropping the local
APIC end of interrupt does the same after the first tick.

## Deferred work

`docs/LEGACY_RETIREMENT.md` covers the next increment, which masks the 8259 pair
permanently and stops the local APIC carrying its output. Level-triggered
routing needs I/O APIC directed EOI. Replacing the PIT with the local APIC timer
needs its own calibration and acceptance test, and only then does the legacy
timer proof become redundant.

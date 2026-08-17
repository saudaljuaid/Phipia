# Retiring the legacy interrupt path

Seneri booted on hardware the PC has carried since 1981 and has been moving off
it one proved layer at a time. This increment closes that path: once the
discovered route is proved, the 8259 pair is masked and latched shut, and the
local APIC stops carrying its output.

## Why retirement is a latch, not a mask

Masking the 8259 pair is a state that any later line of code could undo, by
accident or by habit. Retirement is therefore one way: `pic_retire` masks every
line and sets a latch, and every later `pic_set_mask` is refused with
`PIC_STATUS_RETIRED`, as is a second retirement. That refusal is what turns "the
8259 pair delivers nothing" from a hope into a property the kernel enforces.

The masks are read back from the interrupt mask registers rather than from
Seneri's shadow copy, because what matters is what the hardware holds, not what
Seneri believes it wrote.

## Why LINT0 goes with it

Virtual wire mode existed only to carry the 8259 pair's output to the processor
after the local APIC was enabled. With the pair retired, an `ExtINT` LINT0 is a
live path for interrupts Seneri has decided not to accept, so it is masked like
every other unused local vector table entry, with a legal vector so an
accidental unmask cannot raise an illegal-vector error. LINT1 stays NMI.

Retirement is refused unless the local APIC is online, since masking LINT0 while
the APIC is offline would leave the processor with no interrupt path at all.

## Order matters

Normal boot proves the legacy route, proves the I/O APIC route, retires the
legacy path, and then proves the I/O APIC route again. The last step is the
point of the increment: it demonstrates that retirement removed the inherited
path without taking interrupt delivery with it.

Retiring before proving the replacement would leave a boot that cannot tell a
working I/O APIC from a broken one, because nothing would be delivering
interrupts either way.

## What remains of the 8259

The pair is still initialized during early boot, still remapped away from the
CPU exception vectors, and its spurious IRQ7 and IRQ15 paths are still proved
before retirement. Those paths exist because a spurious interrupt can arrive
from a masked line, and the dispatcher must handle one deterministically for as
long as the hardware is present at all.

What the pair no longer has is a route to the processor.

## Executable proof

The `retired` QEMU scenario proves the property end to end: the pair starts
initialized and unretired, retirement succeeds, every line reads back masked, a
later unmask and a second retirement are both refused, the local APIC no longer
reports legacy routing, the timer still delivers eight interrupts through the
I/O APIC, and no spurious interrupt arrives.

Normal boot additionally requires:

```text
Seneri OS: legacy 8259 retired
Seneri OS: timer survives legacy retirement
```

A negative control confirms retirement is not cosmetic. Asking for the legacy
timer route after retirement fails at the mask, and the guest panics with
`PIT could not update the PIC mask` rather than quietly delivering interrupts
through hardware Seneri believed it had retired.

## Deferred work

`docs/APIC_TIMER.md` covers the next increment, which calibrates the local APIC
timer against the PIT and runs the timer interrupt on the processor itself. The
PIT stayed on as that calibration reference, and `docs/PIT_RETIREMENT.md` covers
the increment that finally removed it: the 8254 is now stopped, masked and
latched shut in the same way the 8259 pair is here, once both derived clocks have
been recalibrated against the ACPI power management timer. Level triggered
routing still needs I/O APIC directed EOI.

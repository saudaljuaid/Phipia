# Local APIC bring-up

This increment turns the discovered interrupt topology into one running piece of
hardware: the bootstrap processor's local APIC. It enables and programs that
APIC without moving any interrupt source onto it.

## Why firmware is not trusted twice

The MADT says where the local APIC is. `IA32_APIC_BASE` says where the processor
thinks it is. Pyrenis requires both, and requires them to agree, because a
disagreement means one of the two describes a machine Pyrenis is not running on,
and neither is then safe to program from. The same applies to identity: the
running APIC's identifier must appear in the ACPI processor list as enabled.

Bring-up therefore rejects, rather than adapts to, a processor with no on-chip
APIC, an APIC left hardware-disabled by firmware, an APIC already in x2APIC mode
whose register window does not exist, a non-bootstrap processor, a null or
out-of-map base, a base or identifier disagreeing with ACPI, a discrete 82489DX
version, and a local vector table too short to hold the entries Pyrenis programs.

Pyrenis does not write `IA32_APIC_BASE`. An APIC left hardware-disabled cannot be
re-enabled without a reset on some processors, so a machine in that state is
refused rather than coerced. Relocating the window or entering x2APIC mode
belongs to a later increment that can prove the new mapping.

## Register access

The local APIC registers are memory-mapped device state, not memory. Their
values change outside the program's control and each access must reach the
device exactly once in program order, which is what `volatile` expresses in
`apic.c`. The SDM requires naturally aligned 32-bit accesses, and every access
here is one.

The window's memory type is still whatever the firmware MTRRs describe, because
Pyrenis's early identity map cannot yet express per-page cacheability. On the
supported target that region is uncacheable. Pinning it explicitly belongs to
the virtual-memory increment, and this file is the record that it is owed.

## Virtual wire mode

This is the invariant that makes the increment safe. Software-enabling the local
APIC takes the 8259 pair off the processor's direct interrupt path. Unless LINT0
carries the PIC's output as `ExtINT`, every legacy interrupt silently stops
arriving, including the PIT that currently proves interrupt delivery works.

Pyrenis therefore programs LINT0 as `ExtINT` and LINT1 as NMI whenever the MADT's
`PCAT_COMPAT` flag reports a legacy PIC, and masks LINT0 when it does not. Every
local vector table entry Pyrenis does not use is masked into a known state rather
than inherited from firmware, and a masked entry still carries a legal vector so
an accidental unmask cannot raise an illegal-vector error. Entries beyond the
count the version register reports are not touched.

The spurious vector is fixed at `0xFF`, whose low four bits are set as some
processor generations require, and it has a registered handler that counts.
A spurious interrupt during bring-up is a fault, not noise.

Programming order matters: the APIC is software-enabled first, because the SDM
forces every local vector table mask bit while it is software-disabled, so the
entries must be written after the enable to take effect. Bring-up then reads
back the spurious register, both LINT entries, and the identifier, comparing
only the fields Pyrenis wrote, since delivery status and remote IRR belong to the
processor.

## What this increment does not do

It routes no interrupt through the APIC, programs no I/O APIC redirection entry,
starts no APIC timer, and masks no PIC line permanently. The 8259 pair still
delivers every interrupt and the PIT still proves it. What changed is that those
interrupts now arrive through the local APIC's LINT0 instead of the direct pin.

## Executable proof

`apic_self_test` proves the decoders that reject a machine Pyrenis must not
program, driven by synthetic register values rather than hardware: a hardware
disabled APIC, x2APIC mode, a non-bootstrap processor, a null base, a base
outside the early map, a base disagreeing with ACPI, a discrete APIC version, a
local vector table that is too short, an identifier absent from the ACPI list,
an identifier present but not enabled, and a null argument.

The `apic` QEMU scenario proves the hardware path end to end: the APIC is
online, its window is usable, legacy routing is in place, the PIT still delivers
its eight interrupts with the APIC enabled, and no spurious interrupt arrived.

The normal scenario additionally requires:

```text
Pyrenis: local APIC online
Pyrenis: local APIC legacy routing LINT0 ExtINT
```

The virtual wire invariant was confirmed by negative control: masking LINT0
instead of routing it stops PIT delivery and hangs the guest until the scenario
times out. The routing is load-bearing and the scenario detects its absence.

## Deferred work

Pyrenis now has a running local APIC that carries legacy interrupts unchanged.
`docs/IO_APIC.md` covers the next increment, which programs redirection entries
from the discovered overrides and delivers the timer through the I/O APIC.
Masking the 8259 pair permanently follows it, and only after that may the local
APIC timer replace the PIT proof, with its own calibration and acceptance test.

# ACPI table trust boundary

This increment turns the validated RSDP into one bounded path to the Multiple
APIC Description Table. It discovers interrupt-controller metadata without
changing interrupt-controller state.

## System-description table invariants

Zenith follows the RSDT only for an ACPI 1.0 root and otherwise follows the
XSDT selected during RSDP validation. The root signature must agree with that
selection. Its declared length must include the 36-byte ACPI description header,
fit within Zenith's current first-4-GiB identity map, and contain a whole number
of 32-bit RSDT or 64-bit XSDT entries. The complete table checksum must be zero.

Firmware controls every length and pointer in this path. Early discovery
therefore limits a root to 256 entries and any individual table to 1 MiB. These
are Zenith policy bounds, not ACPI architectural limits. They keep every loop
finite until the virtual-memory manager can map firmware tables individually.

Each root entry is decoded from its little-endian byte representation so an
XSDT's naturally four-byte-aligned 64-bit entries do not create an unaligned C
access. Before Zenith examines a referenced signature, the address must be
nonzero and the fixed header must fit in the early map. Before it consumes the
table, the complete declared span and checksum must also be valid.

## MADT invariant

Exactly one referenced table must carry the `APIC` signature. Its length must
include the 44-byte fixed MADT prefix, and every reserved MADT flag must be zero.
Discovery records the table address, revision, OEM identity, local APIC address,
and `PCAT_COMPAT` flag. It deliberately does not parse interrupt-controller
structures or touch local APIC, I/O APIC, PIC, or PIT registers.

The frame allocator continues to treat ACPI reclaimable and NVS memory as
reserved, so the discovered table cannot be recycled after discovery.

## Interrupt-controller topology invariant

Zenith walks the variable-length MADT payload only after the complete table has
passed its signature, length, early-map, and checksum checks. Every record must
contain its two-byte header, declare at least that size, fit completely inside
the MADT, and advance the cursor. A separate policy limit of 1,024 records keeps
the walk finite even if firmware fills the maximum one-MiB table with minimum-
length records. Unsupported types are counted and skipped only after these
bounds have been proven.

The x86 topology parser consumes the ACPI 6.6 Processor Local APIC, I/O APIC,
Interrupt Source Override, Local APIC Address Override, and Processor Local
x2APIC structures. Their multibyte fields are decoded from little-endian bytes;
the parser never relies on packed or unaligned C accesses. Supported records
must have the exact specification length and every reserved field or flag must
be zero.

Usable processors have either `Enabled` or `Online Capable`, never both. Records
with both bits clear are counted but ignored as ACPI requires. Zenith accepts at
most 256 usable processors and rejects duplicate ACPI processor UIDs or APIC
IDs. At least one usable processor must remain.

Zenith accepts at most 16 I/O APICs. Their IDs, nonzero MMIO addresses, and
global-system-interrupt bases must be unique. It accepts at most 16 ISA source
overrides: the bus must be zero, the source must be IRQ0-15, polarity and trigger
encodings must be defined, and both source and target GSI must be unique. At most
one Local APIC Address Override may replace the fixed MADT address, and the
effective address must be nonzero. At least one I/O APIC must be present.

This milestone records topology only. It does not read APIC MMIO registers,
change the APIC base MSR, alter redirection entries, unmask an APIC timer, or
change the legacy PIC/PIT state.

## Executable proof

The in-kernel rejection suite constructs valid RSDT and XSDT graphs and then
proves rejection of a mismatched root signature, partial or excessive root
entries, bad root and child checksums, null and out-of-map table addresses,
short child tables, a missing or duplicate MADT, a short MADT, and nonzero
reserved MADT flags.

A second rejection suite constructs MADTs containing Local APIC and x2APIC
processors, I/O APICs, ISA overrides, an address override, and an unknown record.
It proves bounded progress, exact supported lengths, legal flags and reserved
fields, capacity limits, unique controller and interrupt identities, required
topology, checksum preservation, and correct handling of unusable processors
and unknown records.

The normal QEMU scenario must also walk SeaBIOS's real ACPI tables and emit:

```text
Zenith OS: ACPI MADT verified
Zenith OS: ACPI MADT topology verified
```

## Deferred work

Before activating this topology, Zenith must establish explicit cache-correct
MMIO mappings, verify processor APIC capability, inspect each I/O APIC's version
and redirection-entry count, and prove that GSI ranges cover the selected timer
route without overlap. Only then may it mask the legacy PIC permanently, route
a timer through discovered APIC hardware, and retire the PIT proof.

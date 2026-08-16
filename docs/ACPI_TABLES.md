# ACPI table trust boundary

This increment turns the validated RSDP into one bounded path to the Multiple
APIC Description Table. It discovers interrupt-controller metadata without
changing interrupt-controller state.

## System-description table invariants

Seneri follows the RSDT only for an ACPI 1.0 root and otherwise follows the
XSDT selected during RSDP validation. The root signature must agree with that
selection. Its declared length must include the 36-byte ACPI description header,
fit within Seneri's current first-4-GiB identity map, and contain a whole number
of 32-bit RSDT or 64-bit XSDT entries. The complete table checksum must be zero.

Firmware controls every length and pointer in this path. Early discovery
therefore limits a root to 256 entries and any individual table to 1 MiB. These
are Seneri policy bounds, not ACPI architectural limits. They keep every loop
finite until the virtual-memory manager can map firmware tables individually.

Each root entry is decoded from its little-endian byte representation so an
XSDT's naturally four-byte-aligned 64-bit entries do not create an unaligned C
access. Before Seneri examines a referenced signature, the address must be
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

## Executable proof

The in-kernel rejection suite constructs valid RSDT and XSDT graphs and then
proves rejection of a mismatched root signature, partial or excessive root
entries, bad root and child checksums, null and out-of-map table addresses,
short child tables, a missing or duplicate MADT, a short MADT, and nonzero
reserved MADT flags.

The normal QEMU scenario must also walk SeaBIOS's real ACPI tables and emit:

```text
Seneri OS: ACPI MADT verified
```

## Deferred work

The next ACPI increment must parse the MADT's variable-length records with the
same bounded discipline. Only after Seneri has proved local APIC, I/O APIC, and
interrupt-source-override topology may it mask the legacy PIC permanently,
route a timer through discovered APIC hardware, and retire the PIT proof.

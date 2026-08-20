# ACPI interrupt topology

This increment parses the MADT's variable-length interrupt-controller
structures into a validated description of the machine's interrupt hardware. It
describes that hardware without programming any of it.

## Entry-walk invariants

The recorded MADT is revalidated before it is walked. Discovery does not trust
the descriptor produced by the previous increment: the address must be nonzero,
the declared length must include the 44-byte fixed prefix and stay within
Pyrenis's 1 MiB early table bound and its first-4-GiB identity map, and the
complete table checksum must still be zero.

Each interrupt-controller structure declares its own one-byte type and one-byte
length. A length below the two-byte entry header would not advance the walk, and
a length beyond the declared table would read past it; both are rejected rather
than clamped, so the loop is finite and every read stays inside the table.

Every structure Pyrenis models must declare exactly its architectural length. A
shorter record cannot hold the fields the type promises, and a longer one means
the firmware disagrees with the revision it claims. Types Pyrenis does not model
are counted and skipped, because ACPI adds structure types over time and an
unknown type is not by itself a defect.

## Topology invariants

Processor flags carry only `Enabled` and `Online Capable`; any other bit is
reserved and must be zero. A processor identifier may not repeat. Local APIC and
x2APIC structures populate the same table, so a machine described either way
yields one processor list.

An I/O APIC identifier may not repeat, and its register window must lie inside
the early identity map. A local APIC address override may appear at most once
and is bounded the same way. Firmware that names an APIC register window Pyrenis
cannot address is rejected rather than truncated to something addressable.

Interrupt source overrides are accepted only for the ISA bus, only for its
sixteen legacy IRQs, and only with the four defined MPS INTI polarity and
trigger-mode bits. One source may not be overridden twice. Those three rules
bound the override count by sixteen, which a static assertion ties to the array
size, so the count needs no separate runtime capacity rejection.

Early discovery holds the result in fixed storage: 64 processors and 8 I/O
APICs. These are Pyrenis policy bounds that keep the description allocation-free
until a heap exists. They are not ACPI architectural limits.

A machine must declare at least one enabled processor and at least one I/O APIC.
Pyrenis needs both to route interrupts through discovered hardware later, and a
description missing either is a firmware defect, not a configuration Pyrenis
should silently accept.

Every rejection leaves the caller's topology zeroed. A partially populated
description must never be mistaken for a complete one.

## What this increment does not do

It writes no APIC register, masks no PIC line, and changes no interrupt state.
The 8259 pair still owns interrupt delivery and the PIT still proves it. The
local APIC address is recorded, not mapped; NMI sources are length-checked and
counted, not routed.

## Executable proof

`acpi_topology_self_test` builds synthetic MADTs and proves acceptance of a
reference machine, of an x2APIC-described processor, and of a bounded local APIC
address override. It then proves rejection of a trailing partial entry, an entry
length that runs past the table, an entry length below the header, a declared
length that disagrees with its type, reserved processor flags, a duplicate
processor identifier, a duplicate I/O APIC identifier, a null and an
out-of-map APIC window, a non-ISA override bus, an out-of-range override source,
reserved override flags, a duplicate override source, a duplicate address
override, both policy limits, a machine with no enabled processor, a machine
with no I/O APIC, and each way the recorded MADT can fail revalidation.

The normal QEMU scenario must also walk SeaBIOS's real tables, report at least
one I/O APIC, and emit:

```text
Pyrenis: ACPI topology verified
```

## Deferred work

Pyrenis now knows where its interrupt hardware is and what it must route.
`docs/LOCAL_APIC.md` covers the next increment, which brings the bootstrap
processor's local APIC online against this description. Programming I/O APIC
redirection entries from the discovered overrides, masking the legacy PIC
permanently, and moving the timer proof onto discovered hardware follow it,
each with its own executable proof.

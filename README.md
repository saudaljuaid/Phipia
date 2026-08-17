<p align="center">
  <img src="assets/seneri-logo.png" alt="Seneri OS logo" width="420">
</p>

# Seneri OS

[![verify](https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml/badge.svg)](https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml)

Seneri OS is a new, independent operating system built from first principles. It
is not a Linux distribution and it does not currently promise application or
hardware compatibility with an existing operating system.

The repository is at its foundation stage. It contains a deliberately small
x86_64 kernel seed—not a finished operating system and not a simulation.

## What boots today

GRUB loads a Multiboot2-compliant ELF kernel in 32-bit protected mode. Seneri
then validates the handoff and CPU, identity-maps the first 4 GiB, enables long
mode, installs a known GDT and stack, and transfers control to freestanding C.
The C kernel defensively validates every Multiboot2 tag, constructs a bounded
physical-frame allocator from the firmware memory map, proves allocation and
release, installs a complete IDT and production GDT/TSS, routes fatal CPU
exceptions through deterministic diagnostics, proves recoverable interrupt
entry plus PIT delivery, validates the firmware ACPI root, walks the checksummed
system-description tables to the MADT, parses that table's
interrupt-controller records into a validated processor, I/O APIC, and
interrupt-override topology, brings the bootstrap processor's local APIC online
in virtual wire mode, delivers the timer through a programmed I/O APIC
redirection entry, retires the legacy 8259 pair, calibrates and runs the local
APIC timer, establishes a time-stamp counter that agrees with it, discovers the
ACPI power management timer from the FADT, and confirms that reference — whose
rate is fixed by specification rather than measured — agrees with both
calibrated clocks before halting safely.

The day-one success contract is the serial line:

```text
Seneri OS: day one passed
Seneri OS: memory foundation passed
Seneri OS: never triple fault milestone passed
Seneri OS: ACPI root verified
Seneri OS: ACPI MADT verified
Seneri OS: ACPI topology verified
Seneri OS: local APIC online
Seneri OS: I/O APIC online
Seneri OS: I/O APIC delivered eight interrupts
Seneri OS: legacy 8259 retired
Seneri OS: timer survives legacy retirement
Seneri OS: local APIC timer delivered eight interrupts
Seneri OS: TSC reference established
Seneri OS: ACPI FADT verified
Seneri OS: PM timer independent reference established
```

## Build and prove it

On Ubuntu 24.04 or a compatible Debian-based environment, install:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools qemu-system-x86 xorriso
```

Then run:

```sh
make verify   # clean build plus ELF, Multiboot2, symbol, and W^X checks
make smoke      # run the strict normal-boot QEMU protocol
make qemu-tests # run fourteen deterministic fault and interrupt scenarios
make run      # optional interactive boot
make hooks    # enforce verification in this local clone
```

## Repository map

- `src/arch/x86_64/boot.S` — Multiboot2 header and 32-to-64-bit transition.
- `src/arch/x86_64/interrupts.S` — normalized interrupt entry and fatal probes.
- `src/kernel/interrupts.c` — IDT ownership, dispatch, and fault diagnostics.
- `src/kernel/cpu.c` — permanent GDT, TSS, and emergency IST stacks.
- `src/kernel/pic.c` and `pit.c` — legacy IRQ routing and timer proof.
- `src/kernel/multiboot2.c` — bounded parser for the boot information contract.
- `src/kernel/physical_memory.c` — 4 KiB physical-frame ownership and allocation.
- `src/kernel/acpi.c` — defensive ACPI RSDP validation and root discovery.
- `src/kernel/acpi_tables.c` — bounded RSDT/XSDT walking, MADT and FADT discovery.
- `src/kernel/acpi_madt.c` — bounded MADT record walking and interrupt topology.
- `src/kernel/acpi_util.c` — shared firmware-table primitives and wire sizes.
- `src/kernel/apic.c` — local APIC bring-up, virtual wire routing, and identity.
- `src/kernel/ioapic.c` — I/O APIC redirection entries and ISA override routing.
- `src/kernel/apic_timer.c` — local APIC timer calibration and periodic ticks.
- `src/kernel/tsc.c` — time-stamp counter calibration and duration arithmetic.
- `src/kernel/pm_timer.c` — ACPI PM timer, wrap folding, and bounded waiting.
- `linker.ld` — low-memory ELF layout with separate R, RX, and RW segments.
- `docs/ACPI_TABLES.md` — firmware-table bounds, invariants, and test protocol.
- `docs/ACPI_TOPOLOGY.md` — interrupt-topology invariants and test protocol.
- `docs/LOCAL_APIC.md` — local APIC invariants, virtual wire mode, and proof.
- `docs/IO_APIC.md` — redirection invariants, override routing, and proof.
- `docs/LEGACY_RETIREMENT.md` — how the 8259 pair is latched shut, and proof.
- `docs/APIC_TIMER.md` — why the APIC timer needs calibration, and its proof.
- `docs/TSC.md` — the second clock, why it exists, and what it cannot claim.
- `docs/PM_TIMER.md` — the first unmeasured reference, and the error it found.
- `docs/NEVER_TRIPLE_FAULT.md` — interrupt ABI, invariants, and test protocol.
- `CONTRIBUTING.md` — non-negotiable engineering and commit rules.

## Current boundaries

Every interrupt Seneri owns now arrives through discovered hardware, the timer
interrupt originates in the processor's own local APIC, and three clocks agree
about how long an interval lasted — one of them the ACPI power management timer,
whose rate is fixed by specification and measured against nothing. That
agreement is new: the PM timer's first act was to prove the PIT had been
delivering two interrupts per programmed period, which had left both calibrated
clocks running at half their true rate while still agreeing with each other. The
PIT remains as the reference the other two are calibrated against, and retiring
it comes next. The PM timer is read by polling, so it is not yet a time base
that keeps a clock or delivers an interrupt, and the supported target does not
report an invariant counter. Level-triggered I/O APIC routing still needs
directed EOI. It has
a deliberately narrow single-core interrupt foundation, but no virtual-memory
manager, heap, scheduler, userspace, filesystem, networking, graphics, or
general hardware drivers. Those arrive only after the previous layer has an
executable acceptance test.

Seneri OS is licensed under GPL-3.0; see `LICENSE`.

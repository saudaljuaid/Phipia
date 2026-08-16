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
entry plus PIT delivery, validates the firmware ACPI root, and walks the
checksummed system-description tables to the MADT before halting safely.

The day-one success contract is the serial line:

```text
Seneri OS: day one passed
Seneri OS: memory foundation passed
Seneri OS: never triple fault milestone passed
Seneri OS: ACPI root verified
Seneri OS: ACPI MADT verified
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
make qemu-tests # run eight deterministic fault and interrupt scenarios
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
- `src/kernel/acpi_tables.c` — bounded RSDT/XSDT walking and MADT discovery.
- `linker.ld` — low-memory ELF layout with separate R, RX, and RW segments.
- `docs/ACPI_TABLES.md` — firmware-table bounds, invariants, and test protocol.
- `docs/NEVER_TRIPLE_FAULT.md` — interrupt ABI, invariants, and test protocol.
- `CONTRIBUTING.md` — non-negotiable engineering and commit rules.

## Current boundaries

Seneri now discovers but does not program APIC hardware. It still has a
deliberately narrow single-core interrupt foundation, but no virtual-memory
manager, heap, scheduler, userspace, filesystem, networking, graphics, or
general hardware drivers. Those arrive only after the previous layer has an
executable acceptance test.

Seneri OS is licensed under GPL-3.0; see `LICENSE`.

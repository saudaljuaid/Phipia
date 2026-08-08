<p align="center">
  <img src="assets/zenith-logo.png" alt="Zenith OS logo" width="420">
</p>

# Zenith OS

[![verify](https://github.com/saudaljuaid/Zenith-OS/actions/workflows/verify.yml/badge.svg)](https://github.com/saudaljuaid/Zenith-OS/actions/workflows/verify.yml)

Zenith OS is a new, independent operating system built from first principles. It
is not a Linux distribution and it does not currently promise application or
hardware compatibility with an existing operating system.

The repository is at **day one**. Today it contains a deliberately small
x86_64 kernel seed—not a finished operating system and not a simulation.

## What boots today

GRUB loads a Multiboot2-compliant ELF kernel in 32-bit protected mode. Zenith
then validates the handoff and CPU, identity-maps the first 4 GiB, enables long
mode, installs a known GDT and stack, and transfers control to freestanding C.
The C kernel defensively validates every Multiboot2 tag, constructs a bounded
physical-frame allocator from the firmware memory map, proves allocation and
release, and reports the result through both VGA and COM1 before halting safely.

The day-one success contract is the serial line:

```text
Zenith OS: day one passed
Zenith OS: memory foundation passed
```

## Build and prove it

On Ubuntu 24.04 or a compatible Debian-based environment, install:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools qemu-system-x86 xorriso
```

Then run:

```sh
make verify   # clean build plus ELF, Multiboot2, symbol, and W^X checks
make smoke    # build the ISO and prove the success line appears in QEMU
make run      # optional interactive boot
make hooks    # enforce verification in this local clone
```

## Repository map

- `src/arch/x86_64/boot.S` — Multiboot2 header and 32-to-64-bit transition.
- `src/kernel/kernel.c` — freestanding C entry, VGA console, and serial output.
- `src/kernel/multiboot2.c` — bounded parser for the boot information contract.
- `src/kernel/physical_memory.c` — 4 KiB physical-frame ownership and allocation.
- `linker.ld` — low-memory ELF layout with separate R, RX, and RW segments.
- `grub/grub.cfg` — deterministic boot menu.
- `docs/DAY_ONE.md` — boot invariants, acceptance criteria, and next steps.
- `CONTRIBUTING.md` — non-negotiable engineering and commit rules.

## Current boundaries

There are no interrupts, virtual-memory manager, heap, scheduler, userspace,
filesystem, networking, graphics, or hardware drivers yet. Those arrive only
after the previous layer has an executable acceptance test.

Zenith OS is licensed under GPL-3.0; see `LICENSE`.

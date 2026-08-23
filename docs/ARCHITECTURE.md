<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Architecture

Sapote is a single-core x86_64 kernel built as one fixed-address ELF image. It
boots through Multiboot2, installs its own memory and interrupt foundations,
discovers emulated hardware, and can hand control to the First Light workspace
or one of the bounded QEMU proof scenarios.

This page is the durable map. Source, headers, self-tests, and the Boot Ledger
remain authoritative when implementation details change.

## Boot and CPU boundary

- `src/arch/x86_64/boot.S` enters long mode and transfers control to
  `kernel_main`.
- `src/kernel/multiboot2.c` validates loader-provided memory, framebuffer, and
  command-line records.
- `src/kernel/cpu.c` and `src/arch/x86_64/cpu.S` install descriptor tables,
  privilege selectors, and TSS stacks.
- `src/kernel/boot_plan.c` declares typed boot stages. `boot_ledger.c` computes
  and verifies their canonical dependency order; `kernel.c` executes that plan.

Raw source order is not boot policy. See [`BOOT_LEDGER.md`](BOOT_LEDGER.md).

## Memory and isolation

`physical_memory.c`, `paging.c`, and `heap.c` provide bounded frame allocation,
four-level page tables, guarded allocations, typed device mappings, PAT memory
types, and installed W^X checks. Kernel mappings remain supervisor-only.

`process.c` creates one private address-space shape for the proof programs. The
ELF image is RX/R, writable state and stack are RW/NX, the stack has a guard
page, and teardown restores the kernel CR3 before releasing user resources.

## Interrupts, clocks, and scheduling

The IDT and assembly stubs preserve same-privilege and CPL3 frames. ACPI MADT
data configures the local APIC and I/O APIC; the legacy PIC is retired. Dynamic
vectors support MSI-X devices.

The ACPI PM timer is the independent clock reference. The APIC timer drives
preemption, the TSC supplies a second calibrated counter, and `clock.c` exposes
one monotonic time source. `timer.c` builds bounded deadlines on it.

`thread.c` provides guarded kernel stacks, a small scheduler, and preemption.
Sapote remains single-core and has no userspace scheduler or general process
service.

## Devices and storage

ACPI and PCI discovery validate firmware tables and configuration-space access
before a driver can claim resources. BAR mappings, MSI-X vectors, and DMA
buffers are typed and generation-checked. Without an IOMMU, a bus-mastering
device is still treated as capable of reaching all physical memory.

The current device proofs are deliberately small:

- xHCI: one emulated controller and one endpoint-zero descriptor transfer;
- NVMe: one controller, namespace, queue pair, and bounded read session;
- FAT16: one read-only superfloppy and one canonical root file;
- PS/2: keyboard and three-byte pointer input for the shell and First Light.

There is no storage write path, VFS, hotplug framework, physical-device
passthrough, or general USB class stack.

## Userspace boundaries

The native Ring 3 proof loads one exact ELF64 fixture and returns through a
private interrupt gate. Separately, the Linux compatibility proof programs the
x86_64 `SYSCALL` MSRs and runs two checksum-pinned static BusyBox profiles:
`echo SAPOTE` and `uname -s`.

That surface is not POSIX and is not Sapote's native application ABI. It accepts
only the measured calls, arguments, mappings, output, and lifecycle documented
in [`LINUX_SYSCALL_ABI.md`](LINUX_SYSCALL_ABI.md).

## Rust boundary

C and assembly control the machine. Freestanding Rust parses selected byte
streams that the kernel did not create: packed fonts and logo data, FAT16
metadata, and ELF64 program records. Only validated, pointer-free results cross
back to C. See [`RUST.md`](RUST.md).

## First Light

`framebuffer.c` validates and maps the linear framebuffer. `surface.c` provides
cached clipped drawing and damage tracking; `screen.c` implements text cells.
`keyboard.c`, `pointer.c`, `shell.c`, and `ui.c` form the interactive boundary.

First Light is a fixed workspace with a menu strip, launcher, tool dock,
terminal, Boot Ledger, system, and about panels. It is not a general window
manager. Its design and capture contract are in
[`FIRST_LIGHT.md`](FIRST_LIGHT.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `include/sapote/` | Public kernel subsystem contracts |
| `src/arch/x86_64/` | Entry, interrupts, process entry, syscall entry, context switch |
| `src/kernel/` | Kernel implementation and guest-side tests |
| `src/rust/` | Freestanding bounded parsers and the C ABI |
| `userspace/busybox/` | Pinned configurations, traces, licenses, and source inputs |
| `tools/` | Deterministic asset, fixture, and BusyBox builders |
| `.github/workflows/` | Required build and measured-profile evidence |
| `assets/` | Canonical logo, font license/source, captures, and boot video |

For current behavior, read a subsystem header, its self-test, and then its
implementation. Use `git log -- <path>` for historical reasoning instead of
keeping milestone diaries in the active documentation set.

## Current limits

Sapote has no SMP, networking, IOMMU, writable filesystem, dynamic linker,
signals, general descriptor table, broad hardware support, or stable native
userspace ABI. These are boundaries, not implied features.

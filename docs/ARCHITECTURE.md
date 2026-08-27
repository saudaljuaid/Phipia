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

`paging.c` holds `PAGING_PROCESS_SPACE_SLOTS` such hierarchies at once rather
than one. Every private operation resolves the caller's token to a slot, a
space and its identity-alias narrowings share an index, and a narrowing may
only be undone while it is the newest one owned - which is what stops one
process's teardown from freeing a split page table another still has a leaf in.

## Interrupts, clocks, and scheduling

The IDT and assembly stubs preserve same-privilege and CPL3 frames. ACPI MADT
data configures the local APIC and I/O APIC; the legacy PIC is retired. Dynamic
vectors support MSI-X devices.

The ACPI PM timer is the independent clock reference. The APIC timer drives
preemption, the TSC supplies a second calibrated counter, and `clock.c` exposes
one monotonic time source. `timer.c` builds bounded deadlines on it.

`thread.c` provides guarded kernel stacks, a small scheduler, and preemption.
Sapote remains single-core.

`multiprocess.c` adds a bounded user scheduler above them: up to four processes
exist at once, each with its own hierarchy, image, stack, generation and saved
CPL3 register set, and the processor goes to each runnable one in turn. A
process leaves through the same reviewed gate the Ring 3 proof uses, with its
whole register set saved on the way out and loaded again on the way back. The
schedule is cooperative and a faulting process is terminated without disturbing
its neighbours. See [`MULTIPROCESS.md`](MULTIPROCESS.md).

## Devices and storage

ACPI and PCI discovery validate firmware tables and configuration-space access
before a driver can claim resources. BAR mappings, MSI-X vectors, and DMA
buffers are typed and generation-checked. Without an IOMMU, a bus-mastering
device is still treated as capable of reaching all physical memory.

The current device boundaries are deliberately small:

- xHCI: one emulated controller and one endpoint-zero descriptor transfer;
- NVMe: at most two controllers, one namespace and queue pair each, with
  generation-authenticated 512- or 4096-byte synchronous read/write sessions;
- FAT32: separate immutable-system and writable-data mounts with bounded
  handles, a four-sector cache, and clean-sync persistence;
- FAT16: retained read-only compatibility proofs for historical releases;
- PS/2: keyboard and three-byte pointer input for the shell and First Light.

`driver.c` adds thirteen bounded drivers for real Intel, Realtek, AMD, Cirrus
Logic and Bochs Display Interface devices. Each binds through the same typed
claim and mapping substrate, performs the reset its specification defines,
identifies its device against a property that specification guarantees, and
releases everything. None of them enables bus mastering, so none of them can
reach memory. See [`DRIVERS.md`](DRIVERS.md).

`audio.c` is the exception and says so. High Definition Audio has no register a
driver can ask a codec through, only two rings the controller reads and writes
by bus-mastering DMA, so identifying a codec means letting the device write
kernel memory. The rings are typed DMA allocations, bus mastering is refused
while they still belong to the kernel, and it is withdrawn only after the
engines are stopped and the controller is back in reset - before the memory is
reclaimed, never after. See [`AUDIO.md`](AUDIO.md).

There is no Unix VFS, journal, hotplug framework, physical-device passthrough,
or general USB class stack. The exact FAT32 design and limits are in
[`FAT32.md`](FAT32.md).

## Networking

`virtio_net.c` owns one modern `virtio-net-pci` function through the same typed
PCI, MSI-X and DMA substrate as storage. It exposes fixed split queues and a
generation-checked packet arena to `network.c`; protocol code never touches a
descriptor or device-owned buffer. Reset invalidates sockets and caches before
releasing device resources.

`network.c` supplies bounded Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP and
HTTP state machines. TCP opens in both directions: a listener with a declared
backlog draws its accepted connections from the same eight-slot table an
outbound connection is drawn from, and a segment matching no connection and no
listener is refused with a reset. HTTP can stream through `fat32_fs.c` to the
writable Data volume with synchronized temporary-file replacement.
`network_syscall.c` validates complete user ranges and authenticates process
generations for the experimental native ABI. The Terminal calls the same public
kernel operations.

One receive buffer and one transmit buffer serve the whole stack, so the pump
runs alone: `network_service` refuses recursive entry, and a send raised while a
received frame is still being parsed resolves its hardware address from the
cache or defers, never by pumping the device again. That ordering is what lets
a handler answer the frame it is reading at all.

The Boot Ledger records networking after time, heap, paging, PCI, dynamic
vectors, DMA, interrupts and the closed boot proofs, and before First
Environment construction. NIC absence or link-down is a healthy availability
decision; malformed initialization is a failed stage. See
[`NETWORKING.md`](NETWORKING.md).

## Userspace boundaries

The native Ring 3 proof loads one exact ELF64 fixture and returns through a
private interrupt gate. Separately, the Linux compatibility boundary programs
the x86_64 `SYSCALL` MSRs and runs three checksum-pinned static BusyBox
profiles: `echo SAPOTE`, `uname -s`, and `cat`. First Light's `linux` command
selects one of the three exact root entries on the deterministic read-only
FAT32 system volume attached as an ordinary emulated NVMe namespace. Each
launch validates
CPU-owned bytes, builds a fresh private address space, enters CPL3,
authenticates exact output, and tears the generation down before restoring the
prompt.

Echo and uname remain synchronous. Cat alone may suspend at its measured
`read(0, 0x400001203f00, 4096)` entry. The syscall boundary saves an
authenticated user frame, restores the kernel CR3 and safe launch stack, and
returns to First Light without printing a prompt. Keyboard events then belong
to the bounded foreground line state. A complete line or EOF is revalidated,
copied all-or-nothing into the authenticated RW/NX mapping, and resumes the
same generation immediately after the real `SYSCALL`. The cycle may repeat
only inside the fixed line and byte limits; failure or exit releases the saved
frame, input, output ownership, mappings, and generation.

The v0.8.0 echo, v0.9.0 uname, and v1.1.0 FAT16 fixtures remain independent
historical proof scenarios. v2.0.0 repackages the same exact executable bytes
on the immutable FAT32 system volume without changing their measured ABI. This
surface is not POSIX and is not Sapote's native application ABI. It accepts
only the measured calls, arguments, mappings, input/output relationship, and
lifecycle documented in [`LINUX_SYSCALL_ABI.md`](LINUX_SYSCALL_ABI.md).

## Rust boundary

C and assembly control the machine. Freestanding Rust parses selected byte
streams that the kernel did not create: packed fonts and logo data, FAT16/FAT32
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

Sapote has no SMP, IPv6, TLS, firewall, routing, Wi-Fi, IOMMU, general VFS,
journaled crash recovery, dynamic linker, signals, general descriptor table,
broad hardware support, browser, or generally stable native userspace ABI. User
scheduling is cooperative rather than preemptive, and there is no fork, exec,
process identifier space or inter-process communication. The thirteen bounded
drivers bind and identify their devices; none of them moves data. The HD Audio
driver identifies codecs and plays nothing. These are boundaries, not implied
features.

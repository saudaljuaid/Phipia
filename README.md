<p align="center">
  <img src="assets/zenith-logo.png" alt="Zenith OS logo" width="420">
</p>

# Zenith OS

[![verify](https://github.com/saudaljuaid/Zenith-OS/actions/workflows/verify.yml/badge.svg)](https://github.com/saudaljuaid/Zenith-OS/actions/workflows/verify.yml)

Zenith OS is a new, independent operating system built from first principles. It
is not a Linux distribution and it does not currently promise application or
hardware compatibility with an existing operating system.

The repository is at its foundation stage. It contains a deliberately small
x86_64 kernel seed—not a finished operating system and not a simulation.

## What boots today

GRUB loads a Multiboot2-compliant ELF kernel in 32-bit protected mode. Zenith
then validates the handoff and CPU, temporarily identity-maps the first 4 GiB,
enables long mode, installs a known GDT and stack, and transfers control to
freestanding C.
The C kernel defensively validates every Multiboot2 tag, constructs a bounded
physical-frame allocator from the firmware memory map, proves allocation and
release, installs a complete IDT and production GDT/TSS, routes fatal CPU
exceptions through deterministic diagnostics, proves recoverable interrupt
entry plus PIT delivery, validates the firmware ACPI root, and walks the
checksummed system-description tables to the MADT. It then validates and records
bounded Local APIC/x2APIC processor, I/O APIC, ISA interrupt-override, and Local
APIC address-override topology.
After consuming firmware data, Zenith replaces the bootstrap map with explicit
4 KiB kernel, VGA, Local APIC, and I/O APIC mappings. Hardware-enforced W^X, NX,
supervisor write protection, and UC device cache policy are validated before
fault and interrupt tests continue. Zenith then probes the mapped controllers,
validates their GSI ranges, permanently masks the legacy PIC, and routes PIT
IRQ0 through the discovered I/O APIC to the bootstrap processor's Local APIC.
Zenith then calibrates the Local APIC timer against five accepted samples from
at most seven bounded attempts on that route, masks the PIT route, starts a
dedicated periodic Local APIC vector, and exposes a checked single-core
monotonic timebase. Normal boot then
initializes a fixed 16 MiB kernel-heap window. Heap growth obtains frames from
the existing allocator, maps supervisor RW+NX write-back leaves through the
permanent hierarchy, zeroes them before publication, and uses fixed external
metadata for bounded first-fit splitting and coalescing.
After the heap is valid, Zenith initializes a fixed single-core cooperative
scheduler. It represents the bootstrap task separately and supports at most 16
dynamic ring-zero tasks. Each dynamic task receives one fixed 64 KiB
page-backed RW+NX stack payload between genuinely absent guard pages. A
freestanding AMD64 switch preserves the System V callee-saved registers, and
generation handles plus two-phase exit/reap ownership prevent a task stack from
being reclaimed while it is running. The Local APIC timer remains a time source
only; it does not preempt or select tasks.

The day-one success contract is the serial line:

```text
Zenith OS: day one passed
Zenith OS: memory foundation passed
Zenith OS: never triple fault milestone passed
Zenith OS: ACPI root verified
Zenith OS: ACPI MADT verified
Zenith OS: ACPI MADT topology verified
Zenith OS: virtual memory foundation passed
Zenith OS: APIC interrupt routing verified
Zenith OS: Local APIC timer and monotonic clock verified
Zenith OS: bounded kernel heap verified
Zenith OS: bounded cooperative scheduler verified
```

## Build and prove it

On Ubuntu 24.04 or a compatible Debian-based environment, install:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools qemu-system-x86 xorriso
```

Then run:

```sh
make verify     # clean build, host oracle, ELF, Multiboot2, symbol, and W^X checks
make smoke      # run the strict normal-boot QEMU protocol
make qemu-tests # run fifteen deterministic fault, memory, timer, heap, and scheduler scenarios
make run      # optional interactive boot
make hooks    # enforce verification in this local clone
```

The default QEMU matrix uses 128 MiB. `QEMU_RAM=19M make qemu-tests` is the
lowest whole-MiB configuration supported by the complete fifteen-scenario
protocol: the heap scenario must simultaneously back the full 16 MiB payload
window. The canonical GCC/BIOS run reports 4,266 allocatable frames at 19 MiB.
At 18 MiB it reports 4,010, fewer than the 4,096 simultaneous heap frames that
proof requires, so the scenario deterministically reaches physical
out-of-memory before the required window-exhaustion boundary and is not a
supported full-matrix setup.

## Repository map

- `src/arch/x86_64/boot.S` — Multiboot2 header and 32-to-64-bit transition.
- `src/arch/x86_64/interrupts.S` — normalized interrupt entry and fatal probes.
- `src/kernel/interrupts.c` — IDT ownership, dispatch, and fault diagnostics.
- `src/kernel/cpu.c` — permanent GDT, TSS, and emergency IST stacks.
- `src/kernel/pic.c` and `pit.c` — legacy IRQ routing and calibration reference.
- `src/kernel/multiboot2.c` — bounded parser for the boot information contract.
- `src/kernel/physical_memory.c` — 4 KiB physical-frame ownership and allocation.
- `src/kernel/acpi.c` — defensive ACPI RSDP validation and root discovery.
- `src/kernel/acpi_tables.c` — bounded RSDT/XSDT walking and MADT discovery.
- `src/kernel/acpi_topology.c` — bounded MADT record parsing and topology.
- `src/kernel/virtual_memory.c` — bounded permanent mapping construction.
- `src/kernel/apic.c` — xAPIC routing and calibrated Local APIC timer control.
- `src/kernel/time.c` — saturating monotonic ticks and checked time conversion.
- `src/kernel/heap.c` — transactional page-backed heap ownership and API.
- `src/kernel/heap_core.c` — fixed-descriptor split/coalesce allocator core.
- `src/kernel/scheduler.c` — task-stack transactions, lifecycle, and API.
- `src/kernel/scheduler_core.c` — pure bounded task and ready-ring state machine.
- `src/arch/x86_64/scheduler.S` — cooperative AMD64 context switching.
- `linker.ld` — low-memory ELF layout with separate R, RX, and RW segments.
- `docs/ACPI_TABLES.md` — firmware-table bounds, invariants, and test protocol.
- `docs/VIRTUAL_MEMORY.md` — page permissions, cache policy, and CR3 proof.
- `docs/APIC_ROUTING.md` — controller activation, GSI, and EOI invariants.
- `docs/LOCAL_APIC_TIMER.md` — calibration, timer, and timebase invariants.
- `docs/KERNEL_HEAP.md` — heap window, transactions, failure, and test protocol.
- `docs/KERNEL_SCHEDULER.md` — task states, guarded stacks, switch ABI, and reaping.
- `docs/NEVER_TRIPLE_FAULT.md` — interrupt ABI, invariants, and test protocol.
- `CONTRIBUTING.md` — non-negotiable engineering and commit rules.

## Current boundaries

Zenith now owns the discovered xAPIC interrupt path, uses a calibrated Local
APIC timer for a single-core monotonic clock, and has one bounded page-backed
kernel heap plus one bounded cooperative ring-zero scheduler. The PIT remains
only as the boot calibration reference. Free heap
space is reusable, but committed pages do not shrink back to the frame
allocator. The heap-only leaf mapper is not a general virtual-memory manager.
There is no post-boot drift correction, timer-driven preemption, SMP, dynamic
interrupt-vector allocation, userspace, process or syscall ABI, blocking or
sleeping, filesystem, networking, graphics, or general hardware-driver
framework. A task that never yields retains the CPU. Those capabilities require
separate executable acceptance contracts.

Zenith OS is licensed under GPL-3.0; see `LICENSE`.

<p align="center">
  <img src="assets/sapote-logo.png" alt="Sapote mark" width="220">
</p>

<h1 align="center">Sapote</h1>

<p align="center">
  <strong>A freestanding x86_64 operating system.</strong><br>
  Kernel, drivers, tests, and graphical workbench in one repository.
</p>

<p align="center">
  <a href="https://github.com/saudaljuaid/Sapote/actions/workflows/verify.yml"><img src="https://github.com/saudaljuaid/Sapote/actions/workflows/verify.yml/badge.svg" alt="verify"></a>
  <img src="https://img.shields.io/badge/machine-x86__64-2F8B8C" alt="x86_64">
  <img src="https://img.shields.io/badge/kernel-C11%20%2B%20assembly-233A68" alt="C11 and assembly">
  <img src="https://img.shields.io/badge/parsers-Rust-7B5B89" alt="Rust parsers">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--only-4F8A5B" alt="GPL-3.0-only"></a>
</p>

<p align="center">
  <img src="assets/sapote-first-light.png" alt="Sapote First Light desktop shell" width="820">
</p>

<p align="center"><sub>A real 1024×768 Sapote First Light boot captured from QEMU.</sub></p>

## About

Sapote is an independent, freestanding operating system—not a Linux
distribution and not a userspace simulation. It boots through Multiboot2,
enters long mode, discovers platform hardware, installs interrupt and timekeeping
paths, and manages its own memory.

First Light is a fixed kernel workbench with a patterned blue-gray desktop,
beveled utility windows, bitmap type, direct launchers, and status indicators.
The Boot Ledger checks installed state. Thirty-nine deterministic QEMU
scenarios cover boot, memory, interrupts, devices, the interface, and two
bounded static-BusyBox Linux ABI proofs.

## Identity

The canonical mark is the supplied 375×332 RGBA source at
[`assets/sapote-logo.png`](assets/sapote-logo.png). The repository preserves its
exact bytes; it is never redrawn, recoloured, cropped, or passed through a
runtime PNG parser. First Light renders its shape as a two-colour bitmap
directly on the grey workbench surface. The complete
asset, palette, voice, and naming contract is in
[`docs/BRAND.md`](docs/BRAND.md).

## What is on the machine today

| Area | Current capability |
| --- | --- |
| Boot and CPU | Protected-mode entry, long mode, GDT, TSS, IDT, exception diagnostics, and a bounded typed Boot Ledger |
| Memory | Firmware memory map, bounded contiguous DMA, four-level paging, W^X, a validated device-window registry and MMIO arena, explicit PAT memory types, guarded heap and stacks |
| Interrupts and time | Local APIC, I/O APIC edge and level routes, dynamic vectors and MSI-X, retired PIC/PIT paths, PM timer, TSC, deadlines |
| Hardware discovery | Checksummed ACPI tables, checked PCI configuration reads/writes, bridge-aware enumeration, sized BAR claims, explicit resource ownership, and bounded xHCI and NVMe controller lifecycles |
| Storage format | Four deterministic read-only FAT16 fixtures; separate Linux ABI roots admit the pinned nine-cluster echo and ten-cluster uname executables parsed in Rust |
| Process foundation | One process at a time in a private four-level W^X address space: the inherited fixed proof image or one of two static position-fixed BusyBox profiles with exact Linux initial stacks |
| Linux ABI increments | Real x86-64 `SYSCALL` through `IA32_LSTAR`, checked `IRETQ` return, profile-specific bounded allowlists, `-ENOSYS` otherwise, and exactly `busybox echo SAPOTE` or `busybox uname -s`; uname uses checked non-partial copy-out of one deterministic UTS record |
| Scheduling | Guarded kernel threads, round-robin switching, and timer preemption |
| Graphics and input | Write-combining RGB framebuffer, cached drawing surface, screen console and shell, PS/2 keyboard/pointer, software cursor, and bounded First Light desktop shell |
| Language boundary | C11 and x86_64 assembly kernel; safe Rust parses kernel-external logo, font, FAT16, and the inherited and BusyBox ELF64 shapes through one reviewed unsafe FFI boundary |
| Proof | Capability-validated boot receipts, installed-state invariants, deliberate fault probes, real VirtIO RNG, xHCI USB descriptor, NVMe/FAT16 DMA/MSI-X, real loaded CPL3 ELF and `SYSCALL` instructions, actual framebuffer screenshots, and 39 deterministic QEMU scenarios |

## Build and run

Ubuntu 24.04 or a compatible Debian-based environment is the reference host:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools \
    qemu-system-x86 xorriso
```

Install Rust through `rustup` first (see [`docs/RUST.md`](docs/RUST.md)), then
add its freestanding target and the repository hooks:

```sh
rustup target add x86_64-unknown-none
make hooks
```

Then choose the verification or boot target:

```sh
make verify       # clean build plus ELF, Multiboot2, symbol, and W^X checks
make qemu-tests   # all 39 deterministic fault, memory, device, process, ABI, and UI scenarios
make smoke        # strict normal-boot contract
make run          # interactive graphical boot
```

The build produces `build/sapote.elf` and `build/sapote.iso`.

## Documentation

- [`docs/DAY_ONE.md`](docs/DAY_ONE.md) — the boot contract and first verified milestone.
- [`docs/BRAND.md`](docs/BRAND.md) — canonical name, source logo, palette, and public naming rules.
- [`docs/BOOT_LEDGER.md`](docs/BOOT_LEDGER.md) — typed startup capabilities, canonical planning, receipts, and installed proof.
- [`docs/FIRST_LIGHT.md`](docs/FIRST_LIGHT.md) — desktop state, layout, dock, panels, damage, installed proof, screenshots, and controls.
- [`docs/UI_FONT.md`](docs/UI_FONT.md) and [`docs/POINTER.md`](docs/POINTER.md) — the licensed packed face and IRQ12 input/cursor path.
- [`docs/VIRTUAL_MEMORY.md`](docs/VIRTUAL_MEMORY.md) — page-table ownership and W^X proof.
- [`docs/DEVICE_WINDOWS.md`](docs/DEVICE_WINDOWS.md) — bounded physical windows and their WB/WC/UC policy.
- [`docs/IO_APIC.md`](docs/IO_APIC.md) — discovered interrupt routing and its controls.
- [`docs/MONOTONIC_TIME.md`](docs/MONOTONIC_TIME.md) — clocks, deadlines, and bounded waits.
- [`docs/PCI_ENUMERATION.md`](docs/PCI_ENUMERATION.md) — hardware discovery through two configuration paths.
- [`docs/PCI_RESOURCES.md`](docs/PCI_RESOURCES.md), [`docs/MSI_X.md`](docs/MSI_X.md), and [`docs/DMA.md`](docs/DMA.md) — claimed BARs, dynamic MSI-X delivery, and bounded DMA ownership.
- [`docs/XHCI_HOST.md`](docs/XHCI_HOST.md) and [`docs/USB_ENUMERATION.md`](docs/USB_ENUMERATION.md) — the bounded xHCI lifecycle and one QEMU endpoint-zero descriptor proof.
- [`docs/NVME_CONTROLLER.md`](docs/NVME_CONTROLLER.md), [`docs/BLOCK_READ.md`](docs/BLOCK_READ.md), and [`docs/NVME_QEMU_PROOF.md`](docs/NVME_QEMU_PROOF.md) — the bounded NVMe lifecycle, one-block read contract, and read-only QEMU proof.
- [`docs/FAT16_READER.md`](docs/FAT16_READER.md), [`docs/FILESYSTEM_FILE_READ.md`](docs/FILESYSTEM_FILE_READ.md), and [`docs/FAT16_QEMU_PROOF.md`](docs/FAT16_QEMU_PROOF.md) — the exact FAT16 parser, one-root-file contract, and four-read installed proof.
- [`docs/ELF64_LOADER.md`](docs/ELF64_LOADER.md), [`docs/PROCESS_ADDRESS_SPACE.md`](docs/PROCESS_ADDRESS_SPACE.md), [`docs/CPL3_INTERRUPT_BOUNDARY.md`](docs/CPL3_INTERRUPT_BOUNDARY.md), and [`docs/PROCESS_QEMU_PROOF.md`](docs/PROCESS_QEMU_PROOF.md) — the bounded executable, private W^X hierarchy, privilege boundary, and installed fixture proof.
- [`docs/LINUX_SYSCALL_ABI.md`](docs/LINUX_SYSCALL_ABI.md), [`docs/LINUX_INITIAL_STACK.md`](docs/LINUX_INITIAL_STACK.md), [`docs/BUSYBOX_REPRODUCIBLE_BUILD.md`](docs/BUSYBOX_REPRODUCIBLE_BUILD.md), and [`docs/LINUX_ABI_QEMU_PROOF.md`](docs/LINUX_ABI_QEMU_PROOF.md) — the first bounded Linux ABI increment, measured BusyBox input, and installed proof.
- [`docs/LINUX_UNAME_ABI.md`](docs/LINUX_UNAME_ABI.md), [`docs/DETERMINISTIC_UTS_RECORD.md`](docs/DETERMINISTIC_UTS_RECORD.md), [`docs/CHECKED_USER_COPYOUT.md`](docs/CHECKED_USER_COPYOUT.md), [`docs/BUSYBOX_UNAME_CONTRACT.md`](docs/BUSYBOX_UNAME_CONTRACT.md), [`docs/LINUX_UNAME_INITIAL_STACK_AND_FIXTURE.md`](docs/LINUX_UNAME_INITIAL_STACK_AND_FIXTURE.md), and [`docs/LINUX_UNAME_QEMU_PROOF.md`](docs/LINUX_UNAME_QEMU_PROOF.md) — the second bounded profile, deterministic UTS copy-out, separate fixture, and controlled robustness matrix.
- [`docs/DEVICE_FOUNDATION_VERIFICATION.md`](docs/DEVICE_FOUNDATION_VERIFICATION.md) — executed controls, proof evidence, accelerator sweeps, and the recorded flake.
- [`docs/THREADS.md`](docs/THREADS.md) — guarded threads, switching, and preemption.
- [`docs/FRAMEBUFFER.md`](docs/FRAMEBUFFER.md), [`docs/SURFACE.md`](docs/SURFACE.md), and [`docs/WRITE_COMBINING.md`](docs/WRITE_COMBINING.md) — pixels, cached drawing, memory types, and fenced presentation.
- [`docs/SCREEN_CONSOLE.md`](docs/SCREEN_CONSOLE.md) and [`docs/SHELL.md`](docs/SHELL.md) — the visible interactive path.
- [`docs/MAP.md`](docs/MAP.md) — the complete source and boot-order map.
- [`docs/WORKING_ON_SAPOTE.md`](docs/WORKING_ON_SAPOTE.md) — build, proof, and contribution workflow.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — project rules and submission expectations.

Every substantial subsystem has its own document under [`docs/`](docs/), with
its invariants, processor rules, failure modes, measurements, and negative
controls.

## Current limits

Sapote is still a foundation-stage kernel. First Light is a fixed kernel
desktop shell, not a window manager or userspace. The current `main` branch is
single-core and has only one synchronous proof process; it has no general
userspace/process service, filesystem/storage or network service, IOMMU
isolation, or application ABI. Its one FAT16 reader and ELF loader are private,
read-only and fixture-bounded.
Hardware evidence is strongest in QEMU, with bare-metal coverage still an
explicit goal.

## Contributing

Small, reviewable increments are welcome. Start with
[`CONTRIBUTING.md`](CONTRIBUTING.md), install the hooks, and keep every new loop
bounded and every new refusal named.

Sapote is licensed under [GPL-3.0-only](LICENSE).

<p align="center">
  <img src="assets/seneri-logo.png" alt="Seneri OS logo" width="150">
</p>

<p align="center">
  <img src="assets/seneri-wordmark.svg" alt="Seneri OS — from first principles" width="560">
</p>

<p align="center">
  <strong>A small x86_64 operating system built from first principles.</strong><br>
  Hardware-aware, proof-driven, and growing into a graphical interactive system.
</p>

<p align="center">
  <a href="https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml"><img src="https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml/badge.svg" alt="verify"></a>
  <img src="https://img.shields.io/badge/architecture-x86__64-6f42c1" alt="x86_64">
  <img src="https://img.shields.io/badge/kernel-C11%20%2B%20assembly-2f81f7" alt="C11 and assembly">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--only-2ea44f" alt="GPL-3.0-only"></a>
</p>

<p align="center">
  <img src="assets/seneri-shell.png" alt="Seneri OS graphical console at the interactive shell" width="820">
</p>

<p align="center"><sub>A real 1024×768 Seneri development build in QEMU. The graphical stack is landing through reviewed increments.</sub></p>

## ✦ What is Seneri?

Seneri is an independent, freestanding operating-system kernel—not a Linux
distribution and not a userspace simulation. It boots through Multiboot2,
constructs its own x86_64 environment, discovers platform hardware, owns its
interrupt and timekeeping paths, and builds a protected memory foundation from
first principles.

Correctness is part of the architecture: important claims are checked at boot,
exercised by deterministic QEMU scenarios, and challenged with deliberate
negative controls.

## ⚙️ What works today

| Area | Current capability |
| --- | --- |
| Boot and CPU | Protected-mode entry, long mode, GDT, TSS, IDT, exception diagnostics |
| Memory | Firmware memory map, physical frames, four-level paging, W^X, guarded heap and stacks |
| Interrupts and time | Local APIC, I/O APIC, retired PIC/PIT paths, PM timer, TSC, deadlines |
| Hardware discovery | Checksummed ACPI root, MADT topology, interrupt overrides, and FADT |
| Proof | Boot-time invariants, deliberate fault probes, and 18 deterministic QEMU scenarios |

Graphical output, PCI discovery, kernel threads, keyboard input, and the shell
shown above are active integration work and land only after their dependency
chain is reviewed and green.

## 🚀 Build and run

Ubuntu 24.04 or a compatible Debian-based environment is the reference host:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools \
    qemu-system-x86 xorriso
make hooks
```

Then choose the level of proof you want:

```sh
make verify       # clean build plus ELF, Multiboot2, symbol, and W^X checks
make qemu-tests   # all deterministic fault, memory, device, and kernel scenarios
make smoke        # strict normal-boot contract
make run          # interactive graphical boot
```

## 🧭 Find your way around

- [`docs/DAY_ONE.md`](docs/DAY_ONE.md) — the boot contract and first verified milestone.
- [`docs/VIRTUAL_MEMORY.md`](docs/VIRTUAL_MEMORY.md) — page-table ownership and W^X proof.
- [`docs/IO_APIC.md`](docs/IO_APIC.md) — discovered interrupt routing and its controls.
- [`docs/MONOTONIC_TIME.md`](docs/MONOTONIC_TIME.md) — clocks, deadlines, and bounded waits.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — project rules and submission expectations.

Every substantial subsystem has its own document under [`docs/`](docs/), where
the invariants, processor rules, failure modes, measurements, and negative
controls live. The README stays an introduction; the documentation carries the
proof.

## 🚧 Current boundaries

Seneri is still a foundation-stage kernel. The current `main` branch is
single-core and kernel-only; it has no userspace, scheduler, filesystem, storage
or network driver, process isolation, or general application ABI. Hardware
evidence is strongest in QEMU, with bare-metal coverage still an explicit goal.

## 💚 Contributing

Small, reviewable increments are welcome. Start with
[`CONTRIBUTING.md`](CONTRIBUTING.md), install the hooks, and keep every new loop
bounded and every new refusal named.

Seneri OS is licensed under [GPL-3.0-only](LICENSE).

<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

Sapote treats tests as bounded evidence. A green run proves the checked
contract under its recorded QEMU and toolchain conditions; it does not imply
untested hardware support or a broader ABI.

## Local gates

```sh
make lint         # repository whitespace policy
make verify       # clean build, host tests, ELF/link/layout checks
make smoke        # normal QEMU boot and transcript
make qemu-tests   # all 39 bounded QEMU scenarios
```

Useful inspection targets:

```sh
make contract-counts
make contract-scenarios
```

`make verify` rejects compiler warnings, undefined symbols, unresolved
relocations, unexpected linker sections, GOT growth, W+X ELF segments or page
mappings, missing architectural instructions, forbidden floating-point/SIMD
kernel code, changed asset/fixture hashes, and failed Rust parser tests.

## QEMU scenarios

The Makefile is the source of truth for the 39 names. They cover:

- exception entry, IST handling, APIC/I/O APIC routing, and legacy retirement;
- clock calibration, deadlines, paging, heap, and guarded threads;
- PCI, framebuffer, surface, screen, keyboard, shell, device windows, and the
  Boot Ledger;
- First Light rendering and interaction;
- MSI-X/DMA, xHCI, NVMe, FAT16, Ring 3 ELF64, and the two measured Linux
  profiles.

Each scenario has a stable guest debug-exit value, expected host status, and
required serial transcript. A scenario target is deliberately not phony so GNU
Make still applies its pattern rule; it creates no same-named file and therefore
runs on every request.

## Device and userspace evidence

Device scenarios use only QEMU-emulated hardware and temporary regular-file
fixtures. Storage is attached read-only. Evidence requires real interrupt/DMA
ownership transitions and complete teardown; a synthetic unit result cannot
substitute for the installed path.

The xHCI, NVMe, filesystem, process, and Linux-profile workflows add focused
matrix checks. BusyBox workflows build twice from clean pinned sources, compare
the binaries byte-for-byte, audit the ELF and exercised instructions, and check
the exact syscall trace. See
[`BUSYBOX_REPRODUCIBLE_BUILD.md`](BUSYBOX_REPRODUCIBLE_BUILD.md).

## Negative controls

An important test must be shown capable of failure. Make one isolated temporary
mutation that violates the claimed invariant, run the narrowest relevant gate,
observe the named refusal, and restore the source. Do not commit the mutation.

Examples include a missing Boot Ledger dependency, one altered fixture byte,
an invalid user range, an RWX mapping, a wrong guest exit value, or one changed
stable screenshot pixel.

## Pull-request evidence

Every pull request should state:

- the commands and CI workflows run;
- the scenario or transcript lines that establish the change;
- any relevant emulator, accelerator, or hardware limitation;
- the most credible failure outside the tested boundary.

The required `build-and-boot` check must be green on the latest commit before
merge. Do not bypass hooks or protected-branch rules.

## Visual captures

```sh
make capture-first-light
make screenshot-proof
make capture-boot-video
```

These targets produce QEMU evidence for the committed First Light images and
20-second boot video. Visual output supplements the installed framebuffer and
transcript checks; it does not replace them.

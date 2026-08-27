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
make qemu-tests   # all 96 bounded QEMU scenarios
```

Useful inspection targets:

```sh
make contract-counts
make contract-scenarios
```

`make verify` rejects compiler warnings, undefined symbols, unresolved
relocations, unexpected linker sections, GOT growth, W+X ELF segments or page
mappings, missing architectural instructions, forbidden floating-point/SIMD
kernel code, changed asset/fixture hashes, nondeterministic FAT32
reconstruction, failed host filesystem checks, and failed Rust parser tests.

## QEMU scenarios

The Makefile is the source of truth for the 96 names. They cover:

- exception entry, IST handling, APIC/I/O APIC routing, and legacy retirement;
- clock calibration, deadlines, paging, heap, and guarded threads;
- PCI, framebuffer, surface, screen, keyboard, shell, device windows, and the
  Boot Ledger;
- First Light rendering, interaction, measured userspace launch, bounded
  foreground stdin, sequential cat launches, and missing-profile recovery;
- MSI-X/DMA, xHCI, NVMe, historical FAT16, Ring 3 ELF64, and the measured Linux
  profiles;
- FAT32 system loading, data mutation, nested traversal, multi-cluster growth,
  random writes, truncation, rename/move, deletion/reuse, full/corrupt/missing
  media, clean-reboot persistence, cache synchronization, immutability, and
  handle generations.
- modern virtio-net discovery, initialization, absence, link-down and reset;
  DHCP/static configuration, ARP, ICMP, UDP, DNS A/CNAME/malformed handling,
  TCP close/retransmission/reset, HTTP framing/redirect/recovery, FAT32 streamed
  download/persistence, syscall isolation and existing-environment regressions.
- several private address spaces live at once, the round-robin schedule, saved
  register sets, isolation, a contained user fault, the address-space slot
  bound and the ordering rule for identity-alias restores;
- thirteen bounded device drivers binding, resetting and identifying real
  Intel, Realtek, AMD, Cirrus Logic and Bochs Display Interface devices, with
  four station addresses pinned on the host command line and required back out
  of the drivers, and with absence reported as absence.

Each scenario has a stable guest debug-exit value, expected host status, and
required serial transcript. A scenario target is deliberately not phony so GNU
Make still applies its pattern rule; it creates no same-named file and therefore
runs on every request.

## Device and userspace evidence

Device scenarios use only QEMU-emulated hardware and temporary regular-file
fixtures. The authenticated system namespace is read-only; each writable
scenario gets a private data-image copy except the controlled persistence
reboot, which reuses its synchronized backing. Evidence requires real
interrupt/DMA ownership transitions and complete teardown; a synthetic unit
result cannot substitute for the installed path.

The xHCI, NVMe, filesystem, process, Linux-profile and networking workflows add focused
matrix checks. BusyBox workflows build twice from clean pinned sources, compare
the binaries byte-for-byte, audit the ELF and exercised instructions, and check
the exact syscall trace. The dedicated First Light interactive-userspace
workflow also builds the three-profile volume twice, runs every scenario,
captures real QEMU media, and preserves the v1.1.0 release evidence. The
dedicated v2.0.0 workflow reconstructs both FAT32 images, runs host positive
and negative checks plus all 96 guest scenarios, captures clean-reboot media,
and assembles exact-commit release evidence. See
[`BUSYBOX_REPRODUCIBLE_BUILD.md`](BUSYBOX_REPRODUCIBLE_BUILD.md).

The v2.1.0 networking workflow self-tests the deterministic peer, runs all 96
scenarios, requires all 34 networking scenarios, reconstructs the production
PCAP through every protocol layer, inspects the synchronized Data image, and
captures an interactive 20–25 second QEMU session. Screenshot/video evidence
supplements the serial, packet and storage proofs; it never substitutes for
them.

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
python3 tools/capture-networking.py --iso build/sapote.iso \
    --system build/userspace/sapote-system-fat32.raw \
    --data build/userspace/sapote-data-fat32.raw \
    --output build/networking-capture
```

These targets produce QEMU evidence for the committed First Light images and
approximately 22-second FAT32 persistence video. Visual output supplements the
installed framebuffer and transcript checks; it does not replace them.

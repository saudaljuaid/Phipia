<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Deterministic FAT16/NVMe process proof

Scenario 37, `process`, is the only installed CPL3 proof. Its guest debug-exit
value is `0x34`; QEMU therefore returns host status 105. Every inherited
scenario name, order, exit and stable line remains unchanged.

## Ordinary-file fixture

`tools/make-elf64-fixture.py` constructs the complete 128-byte ELF from scalar
little-endian fields and a literal independent expected-byte record. It does
not call a compiler or linker. It writes only below `build/`, rejects symlinks
and non-regular outputs, flushes, reopens read-only, byte-compares, decodes every
field independently and verifies the instruction bytes and SHA-256.

`tools/make-process-fixture.py` creates a separate 16 MiB ordinary raw file. It
uses the unchanged v0.6.0 geometry: 4096 logical sectors of 4096 bytes, one
reserved sector, one two-sector FAT, 128 root entries, first root sector 3,
first data sector 4 and one `SAPOTE  BIN` root entry at cluster 2. Its payload
is the independently verified ELF rather than the filesystem scenario's
original deterministic content.

| Artifact | SHA-256 |
| --- | --- |
| 128-byte ELF | `C923A94F08DF64523D3DB701E4F9FC5FF5B51DFC21447E1DC57586D40D42B8A9` |
| 16 MiB process FAT16 image | `5130D78A0FEB51EC410E5CC931A1E6485D96549A726E62BCE95F7D5C18FA2290` |

The v0.6.0 filesystem payload digest remains
`D399F065C9F21E2FD51E2AEADB7768EAB7E6E45E5150F31227C9711934A4D1D3`
and its image remains
`B8FE53B80AAC718B36B545CC7A741ADCA52DF3BFE0DEE580D2A179B49DEBA5AC`.

## QEMU attachment

The scenario regenerates the fixture and attaches it with explicit read-only
file and raw `-blockdev` nodes to the standard emulated NVMe device. Logical
and physical block sizes are 4096; one I/O queue pair and one-entry MSI-X table
match the installed v0.6.0 controller contract. It never mounts the image and
never names a host device, host disk, passthrough device, firmware path or KVM.

The inherited raw NVMe proof and v0.6.0 filesystem-content proof report their
neutral fixture absence in this scenario, so neither misclassifies the ELF
namespace. The private one-file seam reuses the validated FAT/NVMe session,
holds exactly one session open, performs the same four metadata-derived reads,
and copies only canonical `SAPOTE.BIN` bytes after DMA ownership returns to the
CPU. It is private to `process.c` and adds no block, VFS, mount, descriptor,
path, cache or multi-cluster interface.

## Installed evidence

The Boot Ledger has required address-space and ELF64 foundation stages and one
optional-neutral installed proof stage. The installed proof has exactly 18
prerequisites: paging, W^X, physical frames, heap, IDT/TSS/interrupt
controllers, enabled interrupt path, calibrated deadlines, threading,
scheduler, PCI ownership, vectors/MSI-X, DMA, NVMe, FAT16, private one-file
read, address-space foundation and ELF64 foundation. Direct boot-path invocation
is a source-contract failure.

Only the `process` scenario runs the installed proof. Every other boot publishes
neutral process-fixture absence. Installed verification requires exactly one
of success and absence. Success is visible only after CPL3 return, kernel CR3
restoration, complete reverse teardown, stable result installation and equal
pre/post frame, table, DMA, PCI, vector, MSI-X, filesystem, gate and CR3 census.

The stable evidence is deliberately free of addresses, timings, paths, PCI
topology, volume identifiers and generations:

```text
ST PROCESS ELF64 SAPOTE.BIN bytes 128 segments 1 ring 3 address-space private result valid teardown clean robustness 42
```

The frozen 42-control matrix and normative byte details are in
[PROCESS_ELF64_QEMU_PROOF.md](PROCESS_ELF64_QEMU_PROOF.md). The milestone
workflow retains normalized scenario, ten-sweep TCG, accelerator and fixture
digest records for release staging.

<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Linux uname initial stack and fixture

The uname increment has a separate initial-stack contract and a separate
read-only NVMe/FAT16 image. Neither replaces or mutates the v0.8.0 echo stack,
configuration, executable, fixture, transcript, or digest.

## Initial stack

At the 16-byte-aligned entry `%rsp`, ten 64-bit words encode:

```text
3
pointer to "busybox"
pointer to "uname"
pointer to "-s"
0                         argv terminator
0                         empty envp terminator
AT_PAGESZ, 4096
AT_NULL, 0
```

The three NUL-terminated strings occupy exactly 17 bytes in the same private
four-page RW/NX stack. Bounds, subtraction, alignment, word installation,
string installation, canonicality, and every pointer are checked. No pointer
can name kernel, MMIO, DMA, page-table, filesystem, NVMe, guard, or executable
storage. The image needs no `AT_RANDOM`, entropy, credentials, platform, vDSO,
loader, environment, or extra auxiliary entry.

## Deterministic fixture

`tools/make-linux-uname-fixture.py` constructs a 16 MiB unpartitioned FAT16
superfloppy with 4,096-byte sectors. The canonical root entry is `UNAMEBOX`,
archive/read-only proof content starting at cluster 2. The 38,368-byte binary
occupies exactly ten contiguous FAT16 data clusters; unused tail bytes are
zero. The fixture SHA-256 is
`48C3465E924D1D2B3C8AB659D2783CAC4AF57DFD83504606AD0DF8F64D7316E3`.

QEMU attaches this image through one read-only raw block node and a dedicated
NVMe namespace. The kernel opens a private uname read session, reads the BPB,
root, FAT, and ten payload clusters through thirteen bounded commands, and
hands bytes to the CPU-owned Rust validation boundary only after DMA ownership
returns. The parser accepts one canonical root executable and one checked
chain. Cycles, free/reserved/bad clusters, premature/late EOC, overlong chains,
volume escape, and checked-arithmetic failures are named refusals.

The Rust ELF parser then accepts only the measured five-header/four-load
conjunction. Eleven private image pages are installed R, RX, R, and RW/NX as
measured, with zero-filled BSS, no W+X page, and no stale writable executable
alias. Teardown first restores kernel CR3, then reverses stack, mappings,
frames, read session, filesystem, and NVMe/DMA ownership before publishing the
proof result.

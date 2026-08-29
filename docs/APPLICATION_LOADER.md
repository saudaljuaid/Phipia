<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native application loader and security model

`native_process_spawn()` accepts a manifest path on the read-only System FAT32
volume. It reads the fixed binary manifest and named executable once, passes
both byte slices to the safe Rust validator, and installs mappings only after
validation succeeds.

Version 1 admits x86_64 little-endian static `ET_EXEC` images. It permits at
most 32 program headers and 16 `PT_LOAD` segments inside
`0x0000400000000000..0x0000400100000000`. It rejects interpreters, dynamic
segments, relocation sections, an executable stack, W+X loads, unsupported
program types, invalid alignment, wrapped or out-of-file ranges, overlapping
page ranges, an entry outside executable content, invalid TLS, files over
16 MiB, and manifest/executable digest disagreement.

The loader allocates a private page table, copies admitted file bytes through
temporary checked aliases, zeroes BSS, installs final W^X permissions, and
never revalidates the file after admission. Executable pages are immutable.
Anonymous pages, TLS, stacks, and surfaces have distinct mapping kinds. The
main stack is 16 pages with an unmapped guard; created threads receive their
own bounded stack and guard.

Initial registers pass `argc`, `argv`, and environment in `RDI`, `RSI`, and
`RDX`. The deterministic stack contains the manifest arguments, a
`SAPOTE_ABI=1` environment entry, and an auxiliary vector containing page size,
entry address, Sapote ABI version, and TLS image/size/alignment records. All
padding is zero and the resulting stack obeys the x86_64 alignment contract.

Every partial failure unwinds installed pages, aliases, frames, handles, TLS,
and address-space state. A userspace exception marks only its thread/process as
faulted; the scheduler restores the kernel CR3 and FS base before cleanup. A
failed image or crashed application must leave the native resource census equal
to the pre-launch census.


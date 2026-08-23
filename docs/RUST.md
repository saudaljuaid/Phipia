<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Rust in Sapote

Rust has one job in Sapote: validate selected byte streams the kernel did not
produce. C and x86_64 assembly continue to own hardware, page tables, interrupt
entry, context switches, and resource lifecycles.

## Boundary

Rust is used where attacker- or fixture-controlled lengths, offsets, counts,
and encodings dominate the risk. Today it validates:

| Module | Input |
| --- | --- |
| `font.rs`, `ui_font.rs` | packed bitmap-font headers and glyph data |
| `logo.rs` | the deterministic runtime pebble stream |
| `fat16.rs`, `linux_fat16.rs` | FAT16 geometry, chains, root entries, and payload digests |
| `elf64.rs`, `linux_elf64.rs` | bounded native and static BusyBox ELF64 records |
| `abi.rs` | the explicit C/Rust calling boundary and embedded assets |

Rust returns checked scalar metadata and fixed-size records. It does not own
allocation, mappings, DMA, devices, processes, or teardown. C supplies bounded
slices only after it owns the underlying memory; Rust never retains a borrowed
pointer across the call.

## Freestanding build

`src/rust/lib.rs` is compiled as a static library for
`x86_64-unknown-none` with:

- `#![no_std]` and `panic=abort`;
- static relocation and no red zone;
- no MMX, SSE, AVX, or floating-point kernel state;
- warnings denied;
- `unsafe_op_in_unsafe_fn` denied;
- linker rejection of unexpected sections, relocations, GOT growth, and W+X.

The resulting archive links directly into the kernel ELF. There is no allocator,
unwinder, hosted runtime, dynamic loader, or Rust entry point.

## Safety rules

- Keep pointer construction and raw slice creation in `abi.rs`.
- Every `unsafe` block names the condition that makes it sound.
- Validate lengths and arithmetic before indexing or slicing.
- Refuse truncated, overlapping, wrapped, noncanonical, executable-writable,
  or otherwise ambiguous input instead of repairing it.
- Return errors without partially publishing decoded state.
- Test both acceptance and deliberate corruption using host-side Rust tests and
  installed QEMU proofs.

## Why not more Rust?

Rust cannot make port I/O, MMIO, page-table mutation, register programming, or
context switching safe; those operations remain `unsafe` regardless of
language. Rewriting proved C merely to increase the Rust percentage would add
ABI and toolchain surface without reducing the underlying hardware risk.

The boundary may grow when Sapote adds a genuinely untrusted structured stream,
such as network packets or broader USB descriptors. It should not grow because
a machine-facing subsystem happens to need new code.

## Adding a parser

1. Define the exact accepted byte shape and maximum sizes.
2. Add a safe Rust parser with named refusals and host tests.
3. Expose the smallest pointer-free result through `abi.rs`.
4. Let C retain resource ownership and lifecycle control.
5. Add an installed proof and a negative control capable of breaking it.
6. Run `make verify` and the affected QEMU scenarios.

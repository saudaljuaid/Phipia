# Rust in Sapote

Sapote is a C kernel with a small Rust parsing crate. This document argues
where the line goes, and why it is not where people usually put it.

## The rule

> **Rust is for parsing input this kernel did not produce. C is for talking to
> hardware.**

A bounds check the compiler inserts and cannot be talked out of is worth most on
a byte stream from outside, because that is where a missing one stops being a
bug and becomes somebody else's primitive. Image files, filesystem metadata,
USB descriptors, network frames and 802.11 management frames are all that. They
are also, not coincidentally, where the interesting security history of every
operating system lives.

## Where Rust would buy nothing, and why

The intuition that Rust belongs in "the memory code" is worth taking seriously
and then rejecting, because it inverts the actual benefit.

`paging.c` writes page table entries. `pci.c` writes an address to a port and
reads a register. `thread.S` changes the stack pointer out from under a running
function. **Every one of those operations is `unsafe` in Rust** — they are
exactly the operations the safe subset exists to forbid. Rewriting them in Rust
would produce a file where every meaningful line sits inside an `unsafe` block,
the borrow checker supervising the bookkeeping around hardware accesses it
cannot reason about at all. That is not a safety improvement. It is a second
toolchain in the boot path in exchange for a stricter type system on the parts
that were never the risk.

So the split is not "dangerous code in Rust". It is:

| | Language | Why |
| --- | --- | --- |
| Page tables, port I/O, MMIO, context switch | C and assembly | Inherently `unsafe`; Rust adds a toolchain, not a guarantee |
| Fixed-shape firmware tables (ACPI) | C | Bounded, already proved, and rewriting working proved code is churn |
| Decoders of external byte streams | **Rust** | Every length is attacker-controlled; the checks should not be optional |
| FAT16 metadata and file-content validation | **Rust** | Checked external byte slices; only pointer-free values cross back to C |
| ELF64 executable metadata | **Rust** | Exact checked decoding before C can allocate or map executable frames |
| Future: USB descriptors, network and 802.11 frames | **Rust** | Same argument, much larger surface |

That last row is the point. The logo decoder is small; it is here to establish
the toolchain, the build integration and the discipline **before** the layers
that will really need it exist.

## What is actually in Rust today

`src/rust/logo.rs` — the boot logo decoder. It reads a run-length encoded image
whose header, run lengths and pixel count are, in principle, attacker
controlled, and it refuses eight distinct malformations by name.

`src/rust/font.rs` — the bounded reader for the original screen-console font
table. It validates the header and checked glyph ranges on the console hot path.

`src/rust/ui_font.rs` — the First Light `SUF1` reader. The build tool parses the
licensed Spleen BDF; the kernel sees only a 24-byte fixed header and 1,520
bitmap bytes. Rust validates every metric, multiplication, offset and requested
glyph range before C draws it.

`src/rust/fat16.rs` — the v0.6.0 exact FAT16 parser. It decodes BPB, FAT and
32-byte root entries from explicit CPU-owned slices, derives every sector with
checked arithmetic, classifies by cluster count, validates one canonical
one-cluster file and computes its deterministic SHA-256. It has no allocator,
retained pointer or unsafe block and returns only fixed `repr(C)` values.
Its twenty-two mutation families run both through the guest C ABI and as a
host Rust unit test; the large fixture builders are test-only and are not part
of the freestanding image.

`src/rust/elf64.rs` — the v0.7.0 exact executable parser. It accepts only the
independently recorded 128-byte ELF64 `ET_EXEC` image with one RX `PT_LOAD`,
checked lower-half canonical placement and an entry inside its file-backed
bytes. It decodes every scalar with checked little-endian readers, retains no
pointer, and returns one 88-byte fixed `repr(C)` value. Its 26 host/guest
families include every truncation boundary and every rejected identifier,
header, table, extent, alignment, permission, address and entry state. C owns
frame allocation, copying, mappings and lifecycle; see `docs/ELF64_LOADER.md`.

`src/rust/linux_fat16.rs` — the v0.8.0 bounded multi-cluster reader. It
consumes only copied FAT16 geometry and CPU-owned block slices, validates one
canonical `BUSYBOX` root entry, builds a fixed 512-slot checked chain,
translates its nine clusters, and verifies the exact payload digest. It retains
no filesystem or DMA pointer.

`src/rust/linux_elf64.rs` — the v0.8.0 measured BusyBox parser. It accepts the
exact 33,584-byte static `ET_EXEC` header conjunction, no more than eight
program headers, and exactly four `PT_LOAD` segments plus non-executable
`PT_GNU_STACK`. Checked readers and arithmetic reject dynamic, interpreter,
TLS, relocation-dependent, overlapping, wrapped, noncanonical, invalid-BSS,
or W+X shapes. Its result is a pointer-free fixed segment array; C never
duplicates the decode.

`src/rust/abi.rs` — the boundary. Every entry point is `extern "C"`. Unsafe
blocks appear only where an ABI function turns validated C pointers into Rust
slices or writes through validated C pointers, with the caller's obligation
written above each boundary. Past those lines everything is safe Rust and every
index is checked.

`src/rust/lib.rs` — the safe crate root and panic handler. Its one call into C
is implemented in `abi.rs`, so executable unsafe Rust remains confined to the
reviewed FFI module.

The source PNG is committed; the derived stream is not.
`tools/make-logo-asset.py` converts `assets/sapote-logo.png` at build time.
The kernel deliberately carries no PNG or DEFLATE parser, so the general-purpose
format stays outside the image and the boot path receives a small stream it can
validate in one bounded pass.

## How it is built

One `rustc` invocation, no Cargo, no `build.rs`. The crate is a `staticlib`
linked into the same image as the C objects.

The target is `x86_64-unknown-none`, chosen because its constraints match the C
flags exactly — no MMX, no SSE, soft float, no red zone — which is what lets the
two languages share a stack and an interrupt frame without a shim. Warnings are
errors on both sides: `-Werror` for C, `-D warnings` plus
`deny(unsafe_op_in_unsafe_fn)` and `deny(missing_docs)` for Rust.

The bounded Linux parsers also compile with LLVM aggregate-copy thresholds of
1024 bytes. This makes their fixed arrays expand to ordinary stores instead of
introducing hosted `memcpy`/`memset` GOT calls. `make verify` requires an empty
GOT and keeps core bounds-panic and formatting machinery out of the
freestanding image; the existing minimal abort handler remains the final
backstop.

## What linking a second language actually cost

Two things, and both were found by breaking something rather than by reading.

**Rust emits sections the linker script had never seen.** `.data.rel.ro`,
`.llvmbc`, `.llvmcmd` and `.note.gnu.property` arrive with the static library.
`ld` places sections a script does not mention wherever it likes, and the first
Rust build to change size opened a gap between `.data` and `.bss` — which
`linker.ld` already asserted against, so it failed loudly, but only by luck of
which assertion happened to exist.

The fix is not to name those four sections. It is `--orphan-handling=error`:
**a section neither placed nor discarded is now a link error.** The script names
every section it wants, discards the build metadata, keeps the debug information
non-loaded, and nothing can be placed behind its back again.

**A freestanding kernel should have no global offset table**, and now it is
asserted. `__got_end == __got_start` or the link fails. This was added while
chasing the section problem and immediately earned its place — see below.

## Executable proof

`sapote_logo_self_test` runs on every boot beside the C self-tests, driving
malformed blobs built in Rust: a bad magic, a short header, a zero width, a zero
height, a width past the bound, a zero-length run, a run larger than the image,
a run that overruns only because of what preceded it, a truncated blob, a buffer
one pixel short, and trailing bytes after the last pixel. It also checks that
alpha actually blends — a fully transparent run must leave exactly the
background, and the same pixel over black and over white must differ.

Normal boot then decodes the real image, blits it centred, and **reads every
decoded pixel back off the screen** to compare against the decode.

### Negative controls

| Breakage | Observed failure |
| --- | --- |
| the run-length bound is dropped | normal boot fails |
| the header magic is not checked | normal boot fails |
| a normal-boot contract line is renamed | `normal scenario did not complete the integrated production path` |
| **an unchecked index replaces the bounds check** | **the link fails: `the kernel gained a global offset table`** |

The last one is the interesting one, and it says something about this
integration that was not designed in.

Replacing `blob.get(range).ok_or(Status::Truncated)?` with `&blob[range]` does
not produce a kernel that panics at runtime. It produces a kernel that **cannot
be linked**. The unchecked index introduces a reachable panic, the panic path
drags in `core`'s formatting and location machinery, that machinery needs
relocations, and the relocations need a global offset table this kernel asserts
it does not have.

The corollary is visible in the finished image: `nm` on `sapote.elf` finds no
`panic_bounds_check` machinery. Every fallible parser operation returns a
status, the compiler proves no bounds panic is reachable, and `make verify`
asserts that the symbol stays absent. The crate-level panic handler required by
the `no_std` crate may still have a symbol, but no parser operation reaches it.
It is kept because that is a property of the current code rather than a
language guarantee, and a future decoder that introduces a genuine panic path
will pull in the forbidden GOT before it can boot.

## Deferred work

- **Three small assets were only the beginning of the policy.** FAT16 metadata
  is now the first production-shaped test; USB and network streams remain later
  tests.
- **No `alloc`.** The crate has no allocator, so no `Vec` and no `String`.
  Wiring `alloc` to the kernel heap is a small change and should wait until
  something needs it rather than being added because it is possible.
- **The GOT assertion may be too strict.** It is right for today's code and it
  caught a real problem. If legitimate Rust ever needs relocations, extending
  the linker script is a deliberate change with its own review, not a surprise.
- **No Rust in an interrupt handler**, and no Rust that runs before paging.
- **The panic handler has never executed.** See above.
- **Verified under QEMU only**, with one Rust toolchain version.

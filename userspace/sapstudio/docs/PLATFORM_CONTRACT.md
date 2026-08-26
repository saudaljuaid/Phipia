<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Platform contract

SapStudio runs on Sapote. This document records what Sapote provides today,
what SapStudio proved by experiment, and the numbered capabilities Sapote must
grow before an editor can exist on it. It is written against Sapote v1.1.0 at
commit `b260f80`.

Nothing here asks Sapote to become a general-purpose operating system. Each
request is shaped the way Sapote already works: a typed boot-ledger stage, a
measured profile with an allowlist, an installed proof, and a negative control
capable of failing.

## What Sapote provides today

Read from the source, not from expectations:

| Facility | State in v1.1.0 | Source |
| --- | --- | --- |
| Ring 3 execution | One native ELF64 fixture with a private address space, returning through a private interrupt gate | `src/kernel/process.c` |
| Application ABI | None. The only userspace surface is a Linux compatibility boundary admitting three checksum-pinned BusyBox programs | `docs/LINUX_SYSCALL_ABI.md` |
| Userspace syscalls | `arch_prctl`, `set_tid_address`, `brk`, `mmap`, `munmap`, `write`, `writev`, `uname`, `ioctl`, `read`, `exit_group` — each accepted only at measured arguments, for the profile that measured them | `userspace/busybox/*-allowlist.txt` |
| Storage | Read-only FAT16 on emulated NVMe, three frozen root entries, no path API, no VFS, no write path | `src/kernel/filesystem.c` |
| Memory to a program | 12 image pages, 4 stack pages, 2 heap pages, 1 anonymous page — 76 KiB in total | `include/sapote/paging.h` |
| Framebuffer | Kernel-owned, mapped write-combining, presented through a cached surface with damage tracking | `src/kernel/framebuffer.c`, `src/kernel/surface.c` |
| Input | Kernel-owned PS/2 keyboard and three-byte pointer, consumed by First Light | `src/kernel/keyboard.c`, `src/kernel/pointer.c` |
| Time | `clock_monotonic_ns()` from a calibrated TSC, cross-checked against the ACPI PM timer; kernel-only | `src/kernel/clock.c` |
| Threads | Kernel threads with guarded stacks and preemption, capacity 8; no userspace threads | `include/sapote/thread.h` |
| Cores | One | `docs/ARCHITECTURE.md` |
| Audio | None. No driver, no device, no mixer, no clock domain | — |
| GPU | None. No accelerator of any kind | — |
| Networking | None, and SapStudio wants none | — |
| Floating point and SIMD | Not enabled and not preserved. See below | measured |

### The floating-point finding

Sapote never sets `CR4.OSFXSR` or `CR4.OSXSAVE`, never executes `fxsave`,
`xsave`, or any of their partners, and saves no x87 or SSE register state at
interrupt entry, at the syscall boundary, or across a context switch. Its build
rejects any floating-point, MMX, SSE, or AVX instruction found in the kernel
image, and its BusyBox profiles are audited for exercised SIMD instructions and
refused if any appear.

The consequence for an editor is not small. Video and audio work is arithmetic,
and every unit of that arithmetic must currently be scalar and software-float.
SapStudio's first milestones are therefore written to be correct rather than
fast, and the capability that changes this — `SAP-04` below — is the single
highest-value item Sapote can add for SapStudio's sake.

### The memory finding

A Sapote program is given 76 KiB of mapped address space today. One 1920×1080
frame in 8-bit RGBA is 8,294,400 bytes: about 106 times that entire envelope.
One 4096×2160 frame is 33.75 MiB, about 455 times it. A one-second cache of HD
frames at 24 fps is 190 MiB.

An editor is a memory system with a user interface attached. `SAP-03` and
`SAP-12` are consequently as load-bearing as the ABI itself.

## What SapStudio proved by experiment

These are measured results from this repository's toolchain investigation, not
predictions. They fix the shape of the first native program.

**The stock bare-metal Rust target cannot be used.** `x86_64-unknown-none`
declares `position-independent-executables: true` and
`static-position-independent-executables: true`, and a freestanding binary
built for it links as `Type: DYN (Position-Independent Executable file)`.
Sapote's ELF validation refuses interpreter, dynamic, relocation, PIE,
executable-stack, and W+X shapes. A stock-target binary is refused before it is
ever mapped.

**A conforming shape is reachable on the stable toolchain.** Building for
`x86_64-unknown-none` with `-C relocation-model=static -C code-model=large`,
linking with GNU `ld` through `-no-pie`, a fixed-address script, and
`--orphan-handling=error`, produces:

```text
Type:                              EXEC (Executable file)
Entry point address:               0x400001000000
LOAD 0x0000400001000000 R
LOAD 0x0000400001000000 R E
LOAD                    RW
```

with no dynamic section, no relocation records, no undefined symbols, an empty
global offset table, and no MMX, SSE, or AVX instruction in the image. That is
exactly the shape `src/rust/linux_elf64.rs` accepts, and exactly the shape a
new native profile would be measured against.

**The large code model renames sections.** With `-C code-model=large` the
compiler emits `.ltext`, `.lrodata`, `.ldata`, and `.lbss`. A linker script
that names only the ordinary spellings silently drops executable code into a
read-only segment. `--orphan-handling=error` is what catches it — the same
lesson Sapote learned when a Rust static library first opened a gap between
data and bss.

**GNU `ld` is the supported linker.** `ld.lld` refuses `--orphan-handling=error`
unless `.symtab`, `.strtab`, and `.shstrtab` are named by the script. Sapote
links with GNU `ld`; SapStudio does the same rather than maintaining two
scripts.

**The image base is forced, and it forces everything else.** Sapote's
`SAPOTE_EARLY_PHYSICAL_LIMIT` is `0x100000000`: the kernel identity-maps the
whole low 4 GiB, supervisor-only, in every address space. A user image cannot
live below that line, so it lives at `0x0000400001000000` — about 70 TiB —
where no address fits a 32-bit displacement. Three consequences follow, and
none of them is a preference:

- the large code model is required, because the default `kernel` model emits
  32-bit sign-extended references that cannot reach the image;
- link-time optimisation cannot be used against the precompiled `core` and
  `alloc`, which were built with a different code model: the bitcode merge
  fails outright;
- a small global offset table is unavoidable. The precompiled standard library
  is position-independent, and at this distance the linker cannot relax its
  GOT-relative accesses into direct ones. The table is 112 bytes, resolved
  completely at link time, and the audit proves the image still has no dynamic
  section and no relocation record.

The alternative to all three is rebuilding the standard library from source on
a nightly toolchain. SapStudio does not, for the same reason Sapote pins a
stable compiler: a toolchain requirement is a promise to everyone who ever
builds the thing.

**The image already exceeds what Sapote can map.** The current build is 70
pages, 280 KiB in total, against the 76 KiB a Sapote program is given today —
and that is a program with no picture and no interface, whose frames are
sixteen pixels wide because that is what fits. The first thing `SAP-03`
unblocks is not video; it is the program itself.

That number needed taking apart, and taking it apart changed what it means.
`make audit` now runs `tools/footprint.py`, which splits the footprint by
section and attributes every sized symbol to the crate that emitted it:

| Section | Pages | Share |
| --- | --- | --- |
| `.text` | 47 | 68% |
| `.rodata` | 5 | 7% |
| `.bss` | 17 | 25% |

**Sixteen of those seventy pages are one constant.** `sapstudio_rt::HEAP` is
`HEAP_BYTES`, sixty-four kibibytes of static arena, and it is a *reservation*
rather than anything the program contains. Reading the total as "the program
has grown" — which every earlier entry in the table below did — was wrong in a
way that mattered: the code went from twenty pages to twenty-six over the same
period, and sixteen pages never moved at all.

The rest, by crate:

| Pages | Crate |
| --- | --- |
| 16.0 | `sapstudio-rt` — the arena, almost entirely |
| 15.3 | `sapstudio-render` |
| 8.8 | `sapstudio-model` |
| 5.9 | `sapstudio-io` |
| 4.0 | `sapstudio-app` |
| 2.7 | `sapstudio-core` |
| 2.4 | `core` |
| 2.4 | `sapstudio-media` |

`sapstudio_io::format::encode` is the largest single function at 7,194 bytes,
then `Edit::apply` at 6,525 and `Lut3D::look_up` at 5,230. Some pages are not
attributed at all: padding, literals, and anything the symbol table does not
carry a size for.

**The heap is eighty-four per cent of what a Sapote program is given, on its
own.** That reframes the problem: the largest single question about this
image's size is not which crates it links, it is how much arena to reserve —
and that number was chosen before anything measured what the program uses.

How much that lever is worth is measured rather than estimated: halving
`HEAP_BYTES` to thirty-two kibibytes took the image from sixty-one pages to
**fifty-three** when that was measured, and `.text` did not move by a byte.
(Measured at that size rather than carried forward from the forty-two-page
image where it was first measured: what a change costs has no answer without
saying in which program.) Nothing here proposes
doing that — the arena is sized for a program nobody has run — but it says
exactly what the arena costs, which is the first thing anyone deciding will
want.

The use is not measurable today, and that is worth saying plainly rather than
guessing at. `BumpHeap` tracks a high-water mark, but `sapstudio-rt` is linked
only into the freestanding image, and the freestanding image cannot run until
`SAP-01` and `SAP-02` exist. So the arena's actual use is one of the things the
first QEMU run will report, and it is on that run's list.

The growth history, kept because a footprint that moves without anyone noticing
is how a program stops fitting:

| Pages | What was added |
| --- | --- |
| 36 | the slate, the model, the reel, the mixdown |
| 38 | *unattributed* — see below |
| 40 | keyframed parameter curves, and the track opacity that reads them |
| 39 | *down* one, while gaining the fader ramp and sound automation |
| 42 | per-keyframe editing, which `Edit::apply` reaches and so the image links |
| 43 | a grade on a clip: the field, the edit, and the format that carries it |
| 43 | the grade's render node and its wiring: no growth at all, because nothing reached it |
| 60 | the slate rendering a picture, which links `sapstudio-render` for the first time |
| 61 | conforming a sequence to an edit decision list — and **none of it is the module** |
| 63 | wipes: the shape rasteriser, reached this time, and all of it `.text` |
| 65 | soft edges: the moment arithmetic, reached for the same reason |
| 69 | masks: the shape on a clip, its edit, its format, and its rasterisation |
| 70 | one asset per digest, and a location hint beside it |
| 70 | offline media: the slate and the planner's question, for **no growth** |

**And the converse, one commit later.** Wipes cost two pages, every byte of
them `.text`, and `sapstudio-render` went from 11.2 pages to 13.0. The
difference from the entry above is not the size of the code — the shape
rasteriser is smaller than the conform module — it is that `Graph::evaluate`
*calls* it, and the slate calls the graph. The same two facts, measured twice
in opposite directions, are one fact: **the image is what something in it
reaches.**

**A module the image never calls costs it nothing, and that was measured, not
assumed.** Conforming added a module to `sapstudio-io`, and the image grew by
exactly one page — all of it `.rodata`, none of it `.text`. Removing the module
and rebuilding gives a byte-identical image; removing the twelve refusal
strings it added to `IoStatus::describe` is what takes the image back to sixty
pages. `sapstudio_io`'s attributed code did not move by a byte, in either
direction.

So the earlier note that `--gc-sections` changes nothing was right and for a
better reason than it gave: unreached code in an rlib is never pulled in, so
section collection has nothing left to collect. What the image links is what
something in it *calls* — which is why the grade's render node cost nothing
until the slate reached it, and why `sapstudio-render` cost seventeen pages the
day it did.

The corollary is the uncomfortable one: a refusal *string* is not free. Twelve
of them is a page, and this project writes one line of prose for every way each
format can be refused. That is a deliberate trade — R-7.3 says a reader that
cannot name what was wrong cannot be trusted to have checked the others — but
it is now a trade with a number on it.

**The image did not render until it did, and that was worth seventeen pages.**

For most of this branch's life `sapstudio-render` had no symbol in the image at
all — not the graph, not the compositor, not the colour pipeline, not the
lookup tables. The slate exercised the model, the reel, the frame pool and the
test patterns and never called `timeline::render`, so the half of the project
that renders was untested on the target and absent from every footprint
recorded here.

The slate renders now, and `.text` went from 22 pages to 39.

That is the honest number for a program that does what this one is for, and it
is three times the budget rather than twice it. It also settles two claims
written here, both of them mine and both wrong at the time.

The thirty-eight-page row used to say the growth was "the timeline rendering
through the graph, which reaches every node kind and so links the whole colour
pipeline". It cannot have been: linking the graph costs seventeen pages, not
two, and the crate was not linked at all. The row says unattributed now.

And `Node::Look` was deferred for a commit on the reasoning that it would cost
about two pages. In the image as it then stood it cost *nothing*, because
nothing reached it. In the image as it stands now, `Lut3D::look_up` is 5,230
bytes and is linked whether or not anything grades — which is about a page and
a third, and near enough to the original guess.

Both versions of that sentence were wrong, and in the same way: **"what does
this cost" has no answer without "in which program".** The estimate was not
wrong about the code. It was wrong about whether the code was reached, and
nothing but the symbol table could have said.

The row that goes down is the useful one. A whole feature went in — a ramping
fader, a second automation lane, the model and the format behind them — and the
image got *smaller*, because two long functions were split and one duplicated
body became a shared step. Growth is not proportional to features; it is
proportional to distinct code the program reaches, and the same pass that adds
a feature can pay for it by removing repetition.

That is not an argument for relying on it. It is an argument for measuring
after every change rather than assuming the direction, which is now what
`make audit` does.

**The trajectory is the thing to watch, not any single row.** The two real
answers stay what they were. `SAP-03` is the one that fixes it. Splitting the
program so the freestanding image links only what it starts with is the one
that does not need Sapote — and the breakdown above is what that decision has
been missing, since it says which crates are actually worth splitting off and
that the arena is a larger question than any of them.

The waveform summary, the peak file and the lookup tables cost nothing, which
is worth knowing: unreferenced code is not pulled out of an `rlib`, so a module
the freestanding image never calls does not reach the image. What grows the
footprint is code the program *reaches*.

`--gc-sections` was tried and changes nothing: the linker script places
sections explicitly, so there is nothing for the collector to decide. Getting
this number down means either splitting the program so that the freestanding
image links less of it, or `SAP-03`. The second is the answer; the first is
what to do if `SAP-03` is slow.

Two tools read the image independently and agree on the total: `elf-audit.py`
sums the loadable *segments*, `footprint.py` sums the loaded *sections*. They
arrive at 172,032 bytes by different routes, which is a cross-check nobody had
to write.

These are not predictions. `make image` produces the artefact, `make audit`
checks it against R-13.4 and R-13.6, `tools/audit-control.py` proves the audit
can refuse by mutating the image two ways and requiring both to fail, and
`make reproducible` builds it twice into different directories and compares the
bytes.

## The capability ladder

Each item is a request to Sapote, in dependency order. Priority is what it
blocks, not how hard it is. "Measured shape" is the narrowest version that
unblocks the milestone — deliberately smaller than a general facility, because
that is how Sapote grows.

### SAP-01 — Native application ABI

*Blocks: everything. Priority: first.*

A native Sapote syscall surface distinct from the Linux compatibility boundary:
its own entry, its own numbering, its own errno space, its own allowlist, its
own installed proof. The Linux boundary is a measured compatibility artefact
and must not become SapStudio's ABI — widening it to fit an application would
destroy the property that makes it trustworthy.

Measured shape: a syscall entry that authenticates process, generation, CR3,
CPL3 entry, and executable range exactly as `linux_syscall.c` does, with an
initial allowlist of `exit`, `write_console`, and `monotonic_ns`.

### SAP-02 — Loading an application image that is not a pinned fixture

*Blocks: every milestone after the first. Priority: first.*

Today a program is admitted by checksum. An editor is developed, so its image
changes on every commit. Sapote needs a way to admit an image by *shape* —
validated ELF64, `ET_EXEC`, static, non-PIE, W^X, bounded segment count, known
base — with the checksum pinned per release rather than per build.

Measured shape: a `sapstudio` profile whose ELF contract is fixed and whose
digest is a release input, plus a negative control proving a malformed image is
refused at each named stage.

### SAP-03 — General anonymous memory

*Blocks: any frame buffer, any decode, any cache. Priority: first.*

A userspace call that maps N anonymous RW/NX pages at a kernel-chosen address
inside the process address space, and one that releases them. Bounded by a
per-process policy maximum, refused past it, released in full at teardown, and
counted by the same resource census the current proofs use.

Measured shape: `map_anonymous(pages) -> address`, `unmap_anonymous(address,
pages)`, initial maximum 64 MiB, guard pages on both sides of each region.

### SAP-04 — Floating point and SIMD state

*Blocks: all real media performance. Priority: highest value per unit work.*

`CR4.OSFXSR` and `CR4.OSXSAVE` enabled after a CPUID check, an `XSAVE` area per
process and per kernel thread, save and restore at the syscall boundary and at
context switch, and an installed proof that a user program's SSE2 register
contents survive an interrupt and a preemption. Only then may SapStudio's build
stop passing `+soft-float`.

Measured shape: SSE2 first, AVX2 as a separate later profile, each with a
negative control that corrupts the save area and observes the named refusal.

### SAP-05 — Userspace time

*Blocks: playback, scheduling, profiling. Priority: first.*

`clock_monotonic_ns()` exposed to a program, with the same monotonicity
guarantee `clock.c` already proves, and no other clock.

### SAP-06 — Userspace framebuffer surface

*Blocks: any user interface. Priority: second.*

A program obtains a mapped RW/NX pixel surface of a fixed geometry and asks the
kernel to present a damage rectangle from it. The kernel keeps ownership of the
framebuffer, validates the rectangle against the surface, and copies. No shared
mapping of device memory into Ring 3, no compositor, no windows.

Measured shape: `surface_acquire(width, height) -> address`,
`surface_present(x, y, width, height)`, one surface per process, geometry fixed
at acquire, refusal on any rectangle not fully inside it.

### SAP-07 — Input events to a program

*Blocks: interaction. Priority: second.*

A bounded event queue a foreground program may drain: key transitions with
scancodes and modifiers, pointer motion and button transitions. Ownership is
explicit and revocable, exactly like the `linux cat` foreground contract
already is, so the shell can take input back.

### SAP-08 — Writable storage

*Blocks: saving a project. Priority: third.*

The current filesystem is a read-only proof with three frozen root entries. An
editor must write a project file and read it back. This is the largest single
request in this document and should arrive in two steps: first a bounded
single-file rewrite with an all-or-nothing commit and a digest check, then a
real directory and allocation path.

Measured shape, step one: `file_open_fixed(name)`, `file_read(offset, len)`,
`file_write_all(bytes)` against one pre-allocated extent, with the write proven
atomic across a simulated interruption.

### SAP-09 — More than one process, and process lifetime

*Blocks: decoder isolation, background render. Priority: fourth.*

Today one program runs at a time and the kernel tears it down before the prompt
returns. Isolating a decoder — the single most attackable component in an
editor — needs a second address space alive at the same time, with a channel
between them.

### SAP-10 — Userspace threads

*Blocks: parallel decode and render. Priority: fourth.*

Kernel threads exist with capacity 8. A program needs its own, or a way to be
scheduled on several.

### SAP-11 — SMP

*Blocks: real-time playback of anything demanding. Priority: fifth.*

Sapote is single-core. Video decode and render scale with cores more cleanly
than with anything else. Until this exists, SapStudio's job graph is written to
be *order-independent and deterministic* so that the day cores arrive, nothing
in the model has to change.

### SAP-12 — A large address space and many mappings

*Blocks: 4K work, long timelines. Priority: fifth.*

`SAP-03` with a much larger policy maximum, many regions, and a mapping table
that is not a fixed 24-entry array.

### SAP-13 — Audio output

*Blocks: audio monitoring, therefore editing. Priority: fourth.*

Sapote has no audio anything. The smallest useful device is an emulated
Intel HDA or AC'97 output stream with a ring buffer, a period interrupt, and a
presentation clock a program can query. Audio is the hardest real-time contract
in the application, and Sapote's existing DMA ownership discipline is exactly
the right foundation for it.

### SAP-14 — Entropy for a program

*Blocks: dithering, cache salting. Priority: low.*

`src/kernel/virtio_rng_proof.c` already exists. Exposing a bounded
`random_bytes(n)` is a small step.

### SAP-15 — Faults reported to a program

*Blocks: surviving a bad media file. Priority: fourth.*

A decoder that faults should terminate a bounded unit of work, not the editor.
Without process isolation or a fault channel, SapStudio's only defence is that
every parser is written in safe Rust — which is the reason
[`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) makes that non-negotiable.

### SAP-16 — GPU

*Blocks: full-quality real-time playback and effects. Priority: last, and
possibly never.*

An editor can be excellent on the CPU alone; several were. This is recorded so
the render graph keeps a device-agnostic seam, not because it is planned.

### SAP-17 — IOMMU

*Blocks: safe capture hardware. Priority: last.*

Sapote states plainly that a bus-mastering device can reach all physical
memory. That is acceptable for emulated fixtures and unacceptable for capture
hardware. Recorded for completeness.

## What SapStudio does in the meantime

Nothing in the ladder blocks the work that matters most early, because most of
an editor is pure logic over data:

- the time model, the project model, the timeline, and undo/redo are pure and
  are developed and tested on the host with no operating system at all;
- every parser is pure, bounded, and fuzzable on the host;
- the render graph, colour pipeline, and mixer are pure functions from typed
  inputs to typed outputs;
- only presentation, storage, input, and audio touch the platform, and each is
  behind one narrow seam named in [`ARCHITECTURE.md`](ARCHITECTURE.md).

That is the whole reason the language law puts Rust in charge: the majority of
this application can be correct long before the platform can run it.

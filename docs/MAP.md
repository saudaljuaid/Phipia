# Where everything is

Pyrenis is fifty-four source files and thirty-eight documents. This page exists
so you never have to find your way through that by opening files at random.

If you are here because the code looked impenetrable: it is not that you are
missing something. A kernel is dense in a way application code is not, because
almost every line is either a hardware register with a fixed meaning or an
ordering constraint that cannot be reordered. What makes it readable is knowing
*why* before you read *what*, and that is what the documents are for.

## Start here, in this order

1. **`docs/BOOT_LEDGER.md`** — the typed stages, capabilities, canonical order,
   receipts and irreversible transitions. This is boot policy now.
2. **`docs/BRAND.md`** — the one public identity, exact source mark, palette,
   namespaces, artifacts and prompt.
3. **`src/kernel/boot_plan.c`** — the installed descriptors and their private
   execution functions. Read capability edges, not raw declaration order.
4. **`src/kernel/kernel.c`** — validates, executes and proves the installed
   ledger. It contains no subsystem call sequence.
5. **`docs/DAY_ONE.md`** — what the machine looks like at the instant the loader
   hands it over.
6. **`src/kernel/logo.c`** — 39 lines. The smallest complete file in the kernel.
7. **Any one document in `docs/`, then its `.c` file.** Not the other way round.

## What each file is

Sizes are lines, which is a poor proxy for difficulty but a good proxy for how
long you will be in there.

### The sequence

| File | | |
| --- | ---: | --- |
| `kernel.c` | 101 | Reversible console bootstrap, validate/execute/installed-proof boundary, then scenario or shell/UI handoff. |
| `boot_plan.c` | 1899 | The installed descriptors, typed dependency declarations, context population and private stage execution functions. |
| `boot_ledger.c` | 2073 | Pure bounded planning, named refusals, receipts, deterministic fingerprint and installed-ledger verification. |
| `boot_report.c` | 281 | Turns what was discovered into the transcript. Never decides anything. |
| `boot_proofs.c` | 2661 | Every proof and bring-up boot runs. Panics rather than returning a status. |

### Getting off the ground

| File | | |
| --- | ---: | --- |
| `arch/x86_64/boot.S` | 190 | Multiboot2 header, 32-bit entry, the first page tables, the jump to long mode. |
| `multiboot2.c` | 563 | Parsing what the loader left in memory. Refuses malformed input rather than trusting it. |
| `console.c` | 200 | Serial port and VGA text. The only way the kernel speaks until the framebuffer exists. |
| `cpu.c`, `arch/x86_64/cpu.S` | 338 + 322 | Descriptor tables, control registers, and the instructions C cannot express. |

### Not dying

| File | | |
| --- | ---: | --- |
| `interrupts.c` | 549 | The interrupt descriptor table and the dispatcher. Read `docs/NEVER_TRIPLE_FAULT.md` first. |
| `arch/x86_64/interrupts.S` | 327 | The stubs that save state before C can run. The ABI is in the comments. |
| `interrupt_self_test.c` | 162 | Deliberately causing faults to prove they are contained. |

### Knowing what machine this is

| File | | |
| --- | ---: | --- |
| `acpi.c` | 302 | Finding the firmware's root pointer without trusting it. |
| `acpi_tables.c` | 1652 | RSDT, XSDT, FADT, MCFG. Bounds and checksums on everything. |
| `acpi_madt.c` | 1058 | The interrupt topology: which APICs exist and how legacy IRQs were rerouted. |
| `acpi_util.c` | 83 | The checks the above three share. |
| `pci.c` | 1260 | Every device on every bus, read two independent ways so each checks the other; checked writes are exposed only to owners. |

### Device substrate

| File | | |
| --- | ---: | --- |
| `pci_resource.c` | 1149 | Decode-safe BAR sizing, explicit claims, and the bounded supervisor-only MMIO arena. |
| `interrupt_vector.c` | 281 | Audited dynamic vector allocation, exhaustion, and generation-checked release. |
| `msix.c` | 476 | Validated MSI-X table/PBA binding and strict reverse rollback. |
| `dma.c` | 469 | Private-record-validated CPU/device ownership over bounded contiguous frame allocations. |
| `virtio_rng_proof.c` | 669 | Isolated modern VirtIO RNG fixture proving BAR mapping, DMA, MSI-X, and teardown. |

### Interrupt hardware

| File | | |
| --- | ---: | --- |
| `apic.c` | 634 | The local APIC. |
| `ioapic.c` | 1128 | Routing external interrupts to it. |
| `pic.c` | 244 | The 8259 pair, and latching them permanently shut. |

### Telling the time

| File | | |
| --- | ---: | --- |
| `pit.c` | 443 | The 8254. Used to calibrate the others, then retired. |
| `pm_timer.c` | 645 | The ACPI timer — the one reference nothing else calibrated. |
| `apic_timer.c` | 762 | Calibrated against the above; drives preemption. |
| `tsc.c` | 366 | The time-stamp counter, and what it may not claim. |
| `clock.c` | 166 | One monotonic instant, chosen from whichever of the above is best. |
| `timer.c` | 691 | Deadlines. `timer_arm` is what preemption is built on. |

### Memory

| File | | |
| --- | ---: | --- |
| `physical_memory.c` | 747 | Which physical frames exist and which are free, including aligned bounded contiguous extents and usable-range overlap queries. |
| `paging.c` | 3006 | Four-level page tables, W^X, the bounded device-window registry, PAT ownership, and WB/WC/UC memory types. Read `DEVICE_WINDOWS.md` before it. |
| `heap.c` | 792 | A bounded, guarded allocator. The first thing that is not a fixed array. |

### More than one thing at a time

| File | | |
| --- | ---: | --- |
| `thread.c` | 1178 | Threads, guarded stacks, the scheduler, and preemption. |
| `arch/x86_64/thread.S` | 107 | The context switch itself. Six registers and a stack pointer. |

### Pixels

| File | | |
| --- | ---: | --- |
| `framebuffer.c` | 477 | The linear framebuffer, validated field by field, mapped write-combining. |
| `surface.c` | 805 | Cached pixels, clipped primitives, overlap-safe copies, damage, and the WC store fence. |
| `screen.c` | 872 | Text on the framebuffer: retained cells, validated viewports, reflow, wrapping, scrolling, and reading it back. |
| `keyboard.c` | 733 | The 8042 and scancode set 1. The first device a person operates. |
| `pointer.c` | 529 | Shared-8042 auxiliary discovery, IRQ12, three-byte packets, bounded coordinates, and UI event publication. |
| `ui.c` | 1799 | First Light state, pure layout, dock/panels, event queue, damage, cursor, and installed render proof. |
| `ui_font.c` | 307 | Verified Spleen metrics, clipped glyph drawing, representative pixel proof, and named refusals. |
| `shell.c` | 740 | One command parser shared by serial and the First Light Terminal panel. |
| `font.c` | 38 | The C side of the font: names for what the reader can refuse. |
| `rust/font.rs` | 276 | The glyph table reader. Rust, on the first hot path in this kernel. |
| `logo.c` | 39 | The C side of the logo: three lines of glue. |
| `rust/logo.rs` | 329 | The decoder. Rust, because it parses bytes the kernel did not produce. |
| `rust/ui_font.rs` | 265 | Bounded `PUF1` parser and glyph reader for the build-packed Spleen face. |
| `rust/abi.rs`, `rust/lib.rs` | 186 + 44 | What the two languages promise each other. |

### Proving it

| File | | |
| --- | ---: | --- |
| `test.c` | 4658 | The thirty-three QEMU scenarios and what each must print. |
| `self_test.c` | 611 | Subsystem checks over synthetic data; the separate pure ledger planner test lives in `boot_ledger.c`. |

## The boot sequence, in order

`kernel_main` does not express subsystem order. It initializes the reversible
console, runs the pure ledger self-test, builds and validates the complete plan,
executes it, verifies every installed receipt, publishes the read-only ledger
and hands off to a scenario or the shell.

The canonical descriptor sequence is:

    early serial -> interrupt foundation -> pure self-tests
    -> boot information -> firmware discovery -> device windows
    -> interrupt controllers -> physical frames
    -> PAT/CR3 installation -> installed paging proofs
    -> optional independent framebuffer WC proof
    -> paging/heap runtime -> optional framebuffer output
    -> keyboard interrupt path -> optional shell
    -> optional UI font -> pointer decision/outcome -> optional UI layout
    -> early scenario gate
    -> interrupt proofs -> routing -> timer calibration
    -> PCI -> threading -> scheduler
    -> PCI resources -> dynamic vectors -> DMA -> device-substrate proof
    -> closing proofs
    -> optional desktop construction -> activation -> installed UI proof

That order is produced from declared capability edges, bounded phases and stable
stage IDs. Raw descriptor insertion order is not policy. See
`docs/BOOT_LEDGER.md` for every edge and the mandatory prerequisites attached to
PAT/CR3, interrupt enable, framebuffer output, APIC timer and scheduler classes.

## When you want to know

| | |
| --- | --- |
| "what does this subsystem promise?" | the matching `docs/*.md`, and its *Deferred work* list for what it does not |
| "what does this function actually do?" | its `*_self_test` — it is the shortest complete description, because it has to be |
| "why is this line here?" | `git log -S'<the line>' -- <file>` — most non-obvious lines were explained by whoever added them |
| "what is this kernel carrying?" | `docs/DEBT.md`, measured rather than remembered |
| "how do I change something?" | `docs/WORKING_ON_PYRENIS.md` |
| "what is the plan?" | `docs/HARDWARE_AND_APPLICATIONS.md` |

## How to re-measure this page

    wc -l src/kernel/*.c src/arch/x86_64/*.S src/rust/*.rs | sort -rn
    grep -n 'REQUIRED_STAGE\|OPTIONAL_STAGE' src/kernel/boot_plan.c

The second prints raw descriptor declarations. Use the `boot-ledger` scenario
for the canonical installed order: declaration order is deliberately not a
semantic source of truth.

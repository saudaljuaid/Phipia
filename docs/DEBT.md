# What Sapote owes

Every other document here ends with a *Deferred work* list, which is honest but
local: each one knows what its own layer is missing and nothing knows what the
whole thing is carrying. This is that register.

It is measured rather than remembered. Every number below came from the tree at
the commit that added this file, and the commands are given so the next person
can re-measure rather than trust it.

## Verdict

**The engineering discipline held; the structure did not keep up.** Nothing
here is a correctness hole in a shipped layer — the thirty-eight scenarios pass,
`nm -u` is empty, the image has no global offset table, and W^X is enforced by
hardware rather than by a linker script. What slipped is *shape*: one file
absorbing every new proof and a test harness whose contract grew into a wall of
shell assertions. The paging and test-runner signatures have since been paid.

The debt is real but it is the cheap kind. It is written down before it is
expensive rather than after.

**Three of the seven are now paid** — the branch census in §1, the `kernel.c`
split in §2, and the growing signatures in §3 — and all are kept below with
what they cost rather than deleted,
because an entry that predicted its own price and was then proved right is worth
more as a record than as a blank space. §2 in particular warned that deferring
it would make the move bigger, and the next increment added 221 lines before
anyone acted on it.

First Light deliberately does not pretend this debt register became a desktop
roadmap. `ui.c` is one 2,125-line bounded kernel shell and `test.c` is now 5,059
lines. Splitting panel rendering or scenario helpers may improve shape later.
v0.8.0 proves one synchronous static BusyBox invocation through a seven-syscall
Linux allowlist, not a general process or POSIX model;
multi-process service, a window manager, and a compositor remain architectural
layers rather than refactors owed by this milestone.

## Paid on the way in

Found while writing this register, fixed in the same commit.

| | |
| --- | --- |
| **`framebuffer_verify` ran at the wrong moment.** | It was called inside the framebuffer's own proof, and the logo was then blitted through that same mapping — roughly 800,000 further stores that nothing re-checked. Verifying before the last thing that writes through a mapping is verifying the wrong moment, which is precisely what `paging_verify` and `heap_verify` sit at the end of boot to avoid. Moved. A control that disturbs the mapping after the logo is drawn now panics with `framebuffer does not match the address space`; under the old ordering it passed. |
| **`docs/MONOTONIC_TIME.md` was factually wrong.** | It listed as deferred: *"The table is a linear scan over fixed storage, because there is no heap."* `timer.c` has taken that table from the heap since the commit before this session. The code changed and the document did not. Corrected, and split — the storage is fixed no longer, the scan is still linear. |
| **`docs/PIT_RETIREMENT.md` carried a claim already corrected elsewhere.** | That level-triggered routing "gates PCI device interrupts". The current split is explicit: installed MSI-X devices bypass pin routing, while devices without a supported message-signalled capability still need a legacy fallback. |

The second of those is the one worth dwelling on. These documents are the
project's contract — the reason `make verify` means something is that someone
wrote down what it was supposed to mean. A deferred-work entry that has silently
become false is worse than no entry, because it is read as current.

## Outstanding

Ordered by what it costs to leave alone, not by size.

### 1. Integration debt — measured and paid

**The exit-value collision is resolved.** PR #31
(`ioapic: route level-triggered sources with directed EOI`) landed first, so it
keeps `0x22`; later scenarios occupy `0x23` through `0x2E`, and `0x22` is
reserved by name in both `test.c` and the `Makefile`.

**The pile of unmerged branches was not a pile.** `git branch -r` showed
eighteen branches and `git branch -r --no-merged origin/main` showed all
eighteen, which reads as eighteen abandoned lines of work. It was not. Every pull request in this
repository was squash-merged, and a squash merge leaves the original branch tip
unreachable from `main` even though every line of it landed. The reachability
question is the wrong one; the patch question is the right one:

    git cherry origin/main <branch>     # '-' means already upstream, '+' means not

Run across all eighteen, sixteen report every commit already upstream. Of the
remaining two, the historical TSC branch reports one commit not upstream purely
because the patch context shifted — the symbol it adds, `cpu_read_tsc`,
is present in `main` verbatim in `src/arch/x86_64/cpu.S` and declared in
`include/sapote/cpu.h`. Checked by hand rather than trusted.

**At the time of the census exactly two branches carried work not in `main`:**

| Pull request | Change | State |
| --- | --- | --- |
| #31 | level-triggered I/O APIC routing | merged |
| #32 | PCI enumeration | merged |

The other sixteen are the remains of merged or superseded pull requests. Two of
them belong to pull requests closed unmerged as duplicates — #23 and #14 — and
their content reached `main` through #24 and #17 respectively. `git cherry`
agrees.

The sixteen stale branches were deleted in one push once the census above had
been checked three ways; their commits remain recoverable from each pull
request's page. PRs #31, #32, and #33 are now merged; there were no open pull
requests when this registry increment branched from `main`.

**This census was broken before it was believed.** Patch identity is not proof,
so the claim was re-checked a second way and then the checker itself was
checked:

| Control | Result |
| --- | --- |
| For all sixteen branches, does `main` contain every function symbol the branch adds to `src/` or `include/`? | Yes, every one. The claim survives a symbol-level check, not just a patch-ID one. |
| Was #31's work genuinely absent from `main` at census time, so the census was not vacuously true? | It was absent. `acknowledgement_targets_are_resolved`, `directed_eoi_is_gated_on_version` and `entries_round_trip` existed on that branch and nowhere in that snapshot of `main`. |
| Can the checker fail at all? | Yes. Fed `sapote_this_symbol_does_not_exist` it reports missing, so a clean run means something. |

**What remained between #31 and #32 was textual.** Measured with
`git merge-tree --write-tree --name-only`, the two branches touch the same five
files — `Makefile`, `README.md`, `docs/PIT_RETIREMENT.md`, `src/kernel/kernel.c`
and `src/kernel/test.c` — in the same regions: the scenario list, the boot
sequence, and the deferred-work paragraph both changes rewrite.
`include/sapote/test.h` merges cleanly.

None were semantic disagreements. They were resolved when #31 landed before
#32; #33 then landed on their combined `main`. This is retained as the measured
cost that justified paying the integration debt.

### 2. `kernel.c` was the place proofs and order went to live — paid twice

The first payment split proof implementations out of a 2,211-line
`kernel.c`. It left one smaller but still manually ordered call list. That was a
useful boundary and an incomplete policy: ACPI-before-topology,
device-windows-before-paging, W^X-before-heap and controller-before-interrupt
relationships remained understandable only by reading the entire function.

The Sapote Boot Ledger is the second payment:

| | | |
| --- | ---: | --- |
| `src/kernel/kernel.c` | 101 | validates, executes and verifies one ledger; no subsystem call order |
| `src/kernel/boot_plan.c` | 1899 | private stage functions and typed dependency declarations |
| `src/kernel/boot_ledger.c` | 2073 | bounded canonical planner, receipts, fingerprint and installed proof |
| `src/kernel/boot_report.c` | 281 | describes what was found, never decides |
| `src/kernel/boot_proofs.c` | 2661 | existing hardware proofs and transition sequences |

The plan and receipts are fixed-capacity because they authorize the heap and
cannot depend on it. Stable stage IDs, phase bounds and declared capability
edges produce one topological order independent of descriptor insertion order.
Missing providers, ambiguous providers, cycles, premature irreversible stages,
receipt mismatches and bypasses are named refusals. One source assertion keeps
the migrated proof calls private to `boot_plan.c`.

The loader-only structure was renamed `struct boot_information`; the new
`struct boot_context` owns typed discovered state across every stage instead of
growing more arguments or reintroducing file-scope firmware reads. The complete
contract and controls are in `docs/BOOT_LEDGER.md`.

The earlier byte-for-byte transcript control remains relevant and has been
expanded: the permanent installed-ledger proof is a required normal line, and
the comparator is itself broken by one changed word, one deleted proof line and
one changed scenario exit before its output is trusted.

**What this does not fix.** `boot_proofs.c` is 2,661 lines and is now the second
largest file here. It has a single responsibility, which the old `kernel.c` did
not, so it is a better 2,661 lines — but the first option this entry offered,
moving each proof beside the subsystem it proves, is still the better end state
and is still undone.

### 3. Signatures growing a parameter per increment — paid

`paging_initialize` took one argument three commits ago and takes three now:

    paging_initialize(topology)
    paging_initialize(topology, mcfg)
    paging_initialize(topology, mcfg, framebuffer)

Every addition was a *typed physical window* — a range carved out of the bulk
write-back identity map. APIC, VGA, and PCI ECAM are uncacheable; the framebuffer
is write-combining; ordinary RAM stays write-back. The predicted registry now
exists. `paging_initialize` takes one bounded, validated
`struct paging_device_windows`; entries name kind, indexed instance, physical
span, semantic memory type, and semantic access without exposing table bits.

The fixed capacity is twelve: VGA, local APIC, eight bounded I/O APICs, optional
ECAM, and optional framebuffer. Canonical sorting makes insertion order
irrelevant; every duplicate or overlap is a named refusal. `kernel_test_run`
now takes one `struct kernel_test_context`, which carries the same registry and
the explicit optional descriptions scenarios need without hidden reads.

Measured cost: the new public registry and context replaced the three paging
hardware arguments and three test-runner environment arguments, removed five
derived window fields from `paging_state`, added one scenario and one document,
and made the pure registry validator plus installed page-by-page proof part of
every boot.
The executable text crossed one 4 KiB boundary (33 to 34 executable leaves);
the removed fixed fine-region storage reduced BSS by one 4 KiB page, so the
linked image size remained unchanged. The normal transcript otherwise retained
its stable words and mapping/device counts.

### 4. The harness contract is 252 shell assertion lines

    $ grep -c 'grep -F\|grep -E' Makefile
    252

The stale figures before this remeasurement were thirty-one scenarios and 91
matching assertions. The v0.2.0 `main` snapshot already contained thirty-two
scenarios and 150 assertions; the device-foundation contract adds the
thirty-third scenario and reached 166 assertion lines after replacing one
self-referential grep with a derived guest/host exit comparison, including the
executable-text ISA audit. The v0.4.0 xHCI contract adds scenario 34 and brings
the measured line count to 179; the final identity cleanup removes two obsolete
denylist assertions, leaving 177 lines. The v0.5.0 NVMe contract appends
scenario 35 and fourteen controller, proof and exit checks, producing the
former 191-line contract. The v0.6.0 FAT16 increment appends scenario 36 and
twenty-three fixture, source-boundary, exit and transcript checks, producing the
measured 214 lines shown above. The v0.7.0 process increment appends scenario
37 and seventeen executable, source-boundary, exit and transcript checks,
producing the former 231-line contract. The v0.8.0 Linux ABI increment appends
scenario 38 and twenty BusyBox, syscall-entry, source-boundary, fixture, exit
and transcript checks, producing 251 lines; the private paging-failure seam
adds one source-boundary assertion, producing the measured 252 lines shown
above. The harness
was extended, not refactored, so this debt is explicitly **not paid**.

Most of them are one `||`-joined chain checking the normal boot transcript. It
works — renaming any contract line has been shown to turn the suite red, every
time, for every increment. But it is a continuation-backslash away from silently
dropping a check, and this project has already been bitten once by a test that
passed without running.

A file of expected lines and a loop over it would be shorter, readable, and
harder to break by accident.

### 5. Public surface that only tests call

Twenty-five exported functions have no caller outside their own file and the
test suite:

    apic_spurious_count, apic_timer_expiry_count, apic_timer_is_calibrated,
    apic_timer_is_running, cpu_tables_active, cpu_address_on_ist,
    interrupts_validate, interrupts_ready, frame_reserve_range, pci_shutdown,
    pci_is_initialized, pci_config_read_port, pci_config_read_ecam,
    pci_function_count, pic_is_retired, pic_is_initialized, pit_active_route,
    pit_frequency, pit_is_running, pm_timer_nanoseconds_to_ticks,
    timer_stop, timer_is_started, timer_arm, ...

Much of it is deliberate observability, which is fine. But **`timer_arm` was the
deadline layer's primary entry point with nothing in the kernel calling it** —
the only production user of that subsystem was `timer_sleep_ns`. That was worth
knowing before building preemption on top of it, because the API was less
exercised than its test count suggested.

**Resolved, and the warning was earned.** `thread.c` now arms the quantum
through `timer_arm`, so the path has a production caller. The first run of that
caller hung the machine — not because `timer_arm` was wrong, but because the
threads it was arming for started with interrupts disabled. An entry point that
only tests had used met its first real caller and the first real caller had a
bug. That is the shape this entry predicted.

### 6. No host-side Rust test target

The Rust decoder's self-test runs in the kernel on every boot, so it is covered.
But it was *developed* against a host harness that runs in two seconds instead of
a full QEMU boot, and that harness is not in the repository. The next person
debugging a Rust component will rebuild it from scratch.

A `make rust-check` that compiles `src/rust/logo.rs` for the host and runs its
tests is a few lines and pays for itself the first time it is used.

**Partially paid.** `make verify` now compiles and runs committed host harnesses
for the ordinary FAT16 and ELF64 parsers and for both new Linux FAT16 and ELF64
parsers; the Linux ELF harness consumes the exact checksum-pinned BusyBox
binary. The logo and font decoders still lack the general host target described
above, so the register stays open.

### 7. Single-core state, spread wider every increment

    $ mutable statics per file
    paging.c 40, acpi_madt.c 23, pci.c 21, cpu.c 18, timer.c 16, thread.c 16, ...

Every subsystem holds its state in file-scope statics. This is documented as
deferred in every relevant document and it is the right call for a single-core
kernel — but the surface is now twelve files wide, and it grows with each one.
Nothing needs doing yet. What matters is that the day a second processor appears,
this is not a surprise, and the number above is what it will cost.

The v0.7.0 process proof deliberately adds more single-instance static owners:
one process runtime, private page hierarchy, executable-alias token, proof gate
and filesystem session. That is bounded proof state, not a scalable process
table. Concurrency, per-CPU current-process state and TLB shootdown are future
architecture and this milestone does not mark the single-core debt resolved.

The v0.8.0 Linux ABI proof adds one more deliberately singular owner: syscall
CPU state, candidate/installed/running process state, one initial stack, one
bounded heap extent and one stdout sink. That state is generation- and
CR3-authenticated, but it is not per-CPU, schedulable or reusable as a general
Linux process table.

## Not debt

Measured, and healthy:

- **Thirty-eight QEMU scenarios.** Runtime depends on the host; every scenario
  remains bounded, including the 786,432-pixel framebuffer readback.
- **1,942 KiB on-disk kernel ELF**, of which 21.1 KiB is the packed canonical
  Sapote mark (1,988,272 and 21,573 bytes respectively in the measured current
  tree).
- **No `TODO`, `FIXME`, `XXX` or `HACK` anywhere** in `src/`, `include/`, `docs/`
  or the `Makefile`.
- **No undefined symbols, no global offset table, no RWX segment**, all asserted
  at link or build time rather than checked by hand.
- **Every substantial subsystem has a focused document, a self-test, and a
  negative-control table.**

## How to re-measure this

    wc -l src/kernel/*.c | sort -rn | head
    grep -c 'grep -F\|grep -E' Makefile
    grep -rn 'TODO\|FIXME\|XXX\|HACK' src/ include/ docs/ Makefile
    nm -u build/sapote.elf
    git log --oneline origin/main..HEAD | wc -l

And, for §1 — which branch still holds work that `main` does not:

    for b in $(git branch -r --no-merged origin/main | grep -v HEAD); do
        printf '%-48s %s\n' "$b" "$(git cherry origin/main "$b" | grep -c '^+') unlanded"
    done

Read that output with the squash-merge caveat in mind: a `+` means the patch
identity differs, which is *evidence* of unlanded work and not proof of it.
Shifted context produces a `+` for a change that landed. Confirm by looking for
the symbol the commit adds before concluding a branch matters.

This file is worth exactly as much as the last time somebody ran those.

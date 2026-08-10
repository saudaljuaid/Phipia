# Bounded cooperative kernel scheduler

This milestone adds one narrow execution mechanism after the physical-memory,
permanent-mapping, APIC-time, and bounded-heap foundations. A fixed set of
ring-zero tasks may voluntarily yield to one another. This is the smallest
scheduler that can exercise independent stacks and lifecycle ownership without
making an interrupt frame, timer policy, userspace ABI, or SMP claim.

Preemption remains deferred. Local APIC vector `0x30` continues to advance the
monotonic clock and send its existing EOI, but its handler never selects a task
or calls the scheduler. All selection occurs at an ordinary C call boundary.

## Ownership and fixed bounds

The scheduler owns 16 dynamic descriptors and one separately represented
bootstrap task. The bootstrap task uses the linker-owned boot stack; it never
owns a dynamic stack slot or a task-stack frame. Scheduler metadata, candidate
state, mappings under construction, and saved contexts are fixed BSS objects.
No scheduler record is allocated from the heap or stored in caller memory.

The fixed limits are:

| Resource | Bound |
| --- | ---: |
| Dynamic tasks | 16 |
| Explicit bootstrap tasks | 1 |
| Ready-ring entries | 17 |
| Stack payload per dynamic task | 64 KiB, 16 pages |
| Guard pages per dynamic task | 2 |
| Maximum live task-stack backing | 1 MiB, 256 frames |
| Task-stack virtual window | 1,179,648 bytes, 288 pages |

The declared production scheduler objects occupy 6,516 bytes of fixed BSS:
three 736-byte scheduler-core images, sixteen 256-byte runtime records, one
64-byte bootstrap context, a 128-byte transaction frame array, sixteen mapping
flags, and four one-byte lifecycle/IF flags. The physical allocator adds one
128 KiB bitmap for task-stack ownership. Task-stack VM accounting and its test
injection controls add 18 declared bytes. The linker may insert alignment
padding between objects; these figures describe the exact declared objects,
not payload frames or the pre-existing page-table arena. Verification-only
scheduler scenario counters add a small separate fixed test cost.

The worst-case dynamic RAM commitment is 256 ordinary 4 KiB frames. Frames are
obtained only for live dynamic tasks and are returned after safe reaping.

## Stack virtual layout

The complete canonical half-open window is:

```text
[0xFFFFA00000000000, 0xFFFFA00000120000)
```

Slot `i`, for `0 <= i < 16`, begins at
`0xFFFFA00000000000 + i * 0x12000` and contains:

```text
+0x00000  lower guard, absent
+0x01000  64 KiB payload, 16 present pages when the task is live
+0x11000  upper guard, absent
+0x12000  next slot
```

The builder proves canonicality, page alignment, checked non-wrapping
arithmetic, exact last-slot termination, and non-overlap with the kernel image,
bootstrap and IST stacks, static page tables, VGA, APIC MMIO, the heap payload,
both heap guards, and the existing unmapped probe. Each payload leaf is
supervisor-only RW+NX, ordinary write-back memory, and non-global. Both guards
are zero absent leaves, not read-only or poison mappings.

The window requires exactly three additional page-table pages in the existing
64-page permanent arena: one PDPT, one page directory, and one leaf table. The
arena is not enlarged. A pure builder check proves that three total pages are
insufficient and four pages including the root are sufficient for an otherwise
empty hierarchy. Every parent exists before permanent CR3 publication, so the
runtime surface mutates bounded leaves only and cannot allocate a table.

The task-stack map/query/unmap API accepts only a slot and payload-page index.
It cannot select an arbitrary virtual address or permissions. It checks the
slot, page, canonical address, arithmetic, frame alignment and physical width,
parent ownership, absent or exact leaf, exact frame provenance, permission
bits, mapping count, guards, and the complete permanent hierarchy. Successful
leaf writes use a compiler memory barrier and `invlpg` on the exact address.
Unmap requires the exact physical frame.

## Physical-frame identity

`FRAME_OWNER_TASK_STACK` is disjoint from generic and kernel-heap ownership.
The allocator reconstructs all three allocated-owner counts independently from
its bitmaps and requires their sum to equal the total allocation count. A task
frame cannot be generically released or released through the heap identity.
The scheduler validates every live descriptor's 16 distinct frame identities
against the exact 16 leaves. An unused or retired descriptor must have neither
a runtime record nor a mapped payload page.

## State machine and handle provenance

Dynamic descriptors use these explicit states:

```text
unused -> constructing -> ready <-> running -> exited -> reaping -> unused
                                                           `-> retired
```

`poisoned` is a terminal fail-closed state. Construction and reaping occur in a
candidate image and are not legal observable active states. Exactly one task,
including the bootstrap task, is running after initialization. A ready task is
in the ring exactly once; every other state is absent from it.

The fixed FIFO ring stores the bootstrap identity or a dynamic index plus its
generation. Complete validation walks all 17 positions, distinguishes active
and inactive ring cells, rejects duplicates and stale or out-of-range entries,
reconstructs descriptor membership, and counts the single running task. Each
yield appends the current identity and removes the oldest ready identity, which
gives deterministic round-robin order. Yield with an empty ready ring returns
`SCHEDULER_STATUS_NO_RUNNABLE_PEER` without mutation or waiting.

A public dynamic handle is `{ index, uint64_t generation }`. Generation zero is
invalid. Successful reaping increments the generation; a descriptor at
`UINT64_MAX` becomes permanently retired instead of wrapping and making an
ancient handle valid. Null, out-of-range, stale, running, ready, unused, retired,
and double-reap cases have deterministic rejection statuses.

## AMD64 context contract

`src/arch/x86_64/scheduler.S` implements the switch under the freestanding
System V AMD64 ABI and the kernel's no-red-zone rule. It saves and restores RBX,
RBP, R12, R13, R14, R15, RSP, and a continuation RIP. Caller-saved registers,
floating-point, x87, MMX, SSE, and AVX state are not promised or used. The
direction flag is cleared before assembly enters C.

A new stack is zeroed in full, then receives one validated kernel-text return
address at `stack_end - 8`. Its saved RSP is therefore 8 modulo 16. The first
entry trampoline adjusts the stack before calling C, and the return trampoline
routes a returning entry function into the non-returning exit path. A resumed
context may target only the assembly continuation, with its saved C return
address validated inside the exact owned payload. The bootstrap saved RSP is
validated separately inside the linker-owned boot-stack bounds.

The 64 KiB payload leaves headroom for the measured scheduler, heap, VM,
interrupt-dispatch, and scenario call chains plus the normalized 184-byte
interrupt frame. Compiler stack-usage output is part of the verification gate.
This is a bound, not overflow recovery: x86 ring-zero interrupts arriving near
the bottom of a task payload do not automatically switch to a general guard
stack. The adjacent absent guard makes an overrun fault when the next access
crosses the page boundary, but cannot make arbitrary prior corruption safe.

## Interrupt state and timer interaction

Creation, yield, exit, and reap reject interrupt, exception, NMI, and panic
context through the existing dispatcher/panic ownership signal. No scheduler
API is called from vector `0x30` or any assembly-only emergency path. The
existing IST ownership for NMI, machine check, and double fault is unchanged,
as is the 184-byte interrupt-frame ABI.

Every context-switch decision and assembly handoff runs with `RFLAGS.IF=0`.
The suspended `scheduler_yield` call retains its own pre-call IF state and
restores that exact state when its context resumes. A no-peer or validation
return also restores the caller's state. A newly started task enters C with IF
enabled by documented policy. Stack PTE mutation uses the VM subsystem's
bounded IF-disabled section. This is single-core exclusion only; there is no
lock or SMP-safety claim.

Ordinary tasks may call the heap outside scheduler critical and interrupt
context. The integrated proof keeps one allocation live across multiple yields,
checks its payload, frees it, and revalidates heap statistics.

## Creation, publication, and rollback

Creation is a candidate transaction:

1. Validate the active scheduler, ring, handles, VM hierarchy, guards, frame
   allocator, heap, mappings, and independent counts.
2. Validate the output pointer and require the entry address to be executable
   linked kernel text.
3. Reserve the lowest unused descriptor only in a candidate state image.
4. Acquire 16 task-owned frames, map each fixed leaf, and zero all 4 KiB before
   continuing.
5. After all 64 KiB are present and observed zero, construct the initial RSP,
   kernel return trampoline, and context.
6. Revalidate bounds, context provenance, 16 distinct exact mappings, guards,
   owner and mapping counts, and candidate queue statistics.
7. Publish the ready descriptor and handle only after enqueue and full candidate
   validation succeed.

Failure before publication unmaps completed pages in reverse order, invalidates
each leaf, and releases only frames proven unmapped. Active metadata and the
output handle remain explicitly invalid and unpublished. Failure injection
covers every one of the 16
frame-acquisition and 16 mapping boundaries. An uncertain post-PTE-write result
is treated as possibly mapped; the outer transaction must remove the exact leaf
before releasing the frame. Rollback reconstructs the failed candidate/abort
transition and revalidates exact baseline counts. If safe absence, release, or
validation cannot be proved, the scheduler is permanently poisoned.

## Exit and two-phase reaping

Explicit exit and entry-function return use the same non-returning path. With
interrupts disabled, the running dynamic descriptor becomes exited and the
oldest ready context becomes running. Assembly saves the exited context and
switches away before any reclamation is possible. The bootstrap task cannot use
the dynamic exit API.

Another running task must reap the handle. Reap first prevalidates the complete
exited stack and candidate transition. It unmaps all 16 leaves in reverse order
while retaining every frame. If an unmap fails before release, already removed
leaves are restored where exact restoration is provable and the active task
remains exited. Only after every leaf is absent are frames released, the runtime
record cleared, and the generation advanced or retired. A failure after frame
release has begun cannot in general be rolled back; it poisons scheduling and
refuses creation, switching, or reuse rather than claiming reclamation.

## Executable protocol

The independent Python scheduler model runs at least 250,000 deterministic
operations across four fixed seeds. It checks initialization, create/abort,
publish, round-robin yields, exit, reap, stale handles, generation exhaustion,
descriptor limits, reuse, counters, and injected corruption without importing
the C implementation's queue algorithm. Kernel tests add real frames, mappings,
IF behavior, assembly register preservation, stack patterns, heap use, timer
progress, rollback at every page, and descriptor exhaustion.

The fourteen and fifteenth QEMU scenarios are `scheduler` and
`scheduler-guard`. They require exactly:

```text
ZT BEGIN scheduler
ZT PASS scheduler

ZT BEGIN scheduler-guard
ZT PASS scheduler-guard
```

The scheduler scenario writes debug-exit value `0x1D`, observed by the host as
59. It runs three tasks through a deterministic trace, covers explicit and
return exit, reaping and stale handles, slot reuse, 16-descriptor exhaustion,
frame OOM, all creation map failures, uncertain mapping cleanup, context
rejection, required callee-saved registers, task-local data, heap persistence,
and timer/EOI progress. Timer ticks observed before the first yield prove that
time alone does not switch tasks.

The guard scenario selects slot zero's lower guard at
`0xFFFFA00000000000`, proves it is absent, then performs an assembly load. It
requires a supervisor non-present page fault (`P=0 W=0 U=0 RSVD=0 I=0`), exact
CR2, debug-exit value `0x1E` observed as 61, and one matching marker pair.
Normal boot performs one bounded create/yield/return/reap proof and emits:

```text
Zenith OS: bounded cooperative scheduler verified
```

## Worst credible failure and residual risk

The worst credible failure is restoring a corrupt RSP or return address and
executing on another task's stack, or corrupting queue/mapping ownership so a
live stack is overlapped, executable, prematurely reclaimed, or resurrected.
Related failures include a mapped guard, stale TLB alias, double-owned frame,
lost runnable task, IF corruption, missed timer EOI, and a stack overrun that
damages the heap, scheduler, tables, or unrelated kernel memory. Fixed external
metadata, generation handles, complete bounded validation, exact page/frame
provenance, NX payloads, absent guards, candidate publication, reverse rollback, `invlpg`,
two-phase reclamation, and terminal poisoning reduce or detect these risks; no
finite test suite proves their absence.

The scheduler has no stack canary, high-water accounting, unwinder, join, task
cancellation, blocking wait, sleep, per-task deadline, priority, fairness under
a task that never yields, preemption, floating-point context, userspace,
process, syscall, privilege-transition stack, SMP queue, lock, load balancing,
or dynamic VM allocation. TSS.RSP0 remains a future privilege-transition
concern. Storage, networking, graphics, and a general driver framework are also
outside this milestone.

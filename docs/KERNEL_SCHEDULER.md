# Bounded preemptive kernel scheduler

This milestone adds one narrow execution mechanism after the physical-memory,
permanent-mapping, APIC-time, and bounded-heap foundations. A fixed set of
ring-zero tasks may voluntarily yield, wait on one of eight bounded epoch
events, join another task, sleep on the scheduler's monotonic tick domain, or
be cancelled at an explicit cancellation point. They may also be preempted
after a four-tick quantum. Selection honors eight fixed
priority levels with bounded aging. This exercises independent stacks,
lifecycle ownership, explicit blocking, and timer-driven preemption without a
userspace ABI or SMP claim.

Local APIC vector `0x30` requests rescheduling. The actual switch occurs only
at the outermost interrupt exit, after any required EOI and only when interrupt
depth and preempt count are both zero. DPL0 vector `0x31`, which requires no
EOI, provides the same trap-frame path for yield, block, exit, and deferred
guard rescheduling. Eager XSAVE/XRSTOR state follows the same final frame
handoff; Zenith has no lazy `#NM` switching path.

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
| Priority levels | 8, `0` through `7` |
| Epoch events | 8 |
| Stack payload per dynamic task | 64 KiB, 16 pages |
| Guard pages per dynamic task | 2 |
| Maximum live task-stack backing | 1 MiB, 256 frames |
| Task-stack virtual window | 1,179,648 bytes, 288 pages |

The scheduler uses three fixed 1,440-byte core images, sixteen fixed 208-byte
runtime records, bounded transaction arrays, one bootstrap-frame pointer, one
registered scheduler-class spinlock, and bounded counters. Eager xstate owns a
separate bootstrap image, so it does not consume any of the 16 dynamic slots.
Only CPU zero is scheduled in this milestone. No metadata size depends on
runtime task behavior.

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
                                |   ^      |                 `-> retired
                                v   |      +-> join-blocked
                    event-blocked  |      +-> sleeping
                                `---+------------- wake/cancel
```

`poisoned` is a terminal fail-closed state. Construction and reaping occur in a
candidate image and are not legal observable active states. Exactly one task,
including the bootstrap task, is running after initialization. A ready task is
in the ring exactly once; a blocked task is in exactly one event waiter mask
and no ready-ring position; every other state is absent from both structures.

The fixed ring stores the bootstrap identity or a dynamic index plus its
generation. Complete validation walks all 17 positions, distinguishes active
and inactive ring cells, rejects duplicates and stale or out-of-range entries,
reconstructs descriptor, event, join, sleep, and queue membership, and counts the single running
task. Each yield appends the current identity and selects the oldest task with
the highest effective priority. Numeric priority `7` is highest. Every eight
selections a waiting task gains one effective level, capped at `7`, so a ready
task is selected after a finite number of cooperative scheduling decisions
when callers are not continually resetting priorities.
Equal effective priorities retain deterministic ring order. Yield with an
empty ready ring returns
`SCHEDULER_STATUS_NO_RUNNABLE_PEER` without mutation or waiting.

A public dynamic handle is `{ index, uint64_t generation }`. Generation zero is
invalid. Successful reaping increments the generation; a descriptor at
`UINT64_MAX` becomes permanently retired instead of wrapping and making an
ancient handle valid. Null, out-of-range, stale, running, ready, blocked,
unused, retired, and double-reap cases have deterministic rejection statuses.

`scheduler_task_create` remains the default-priority compatibility entry
point. `scheduler_task_create_with_priority`, `scheduler_task_set_priority`,
and `scheduler_task_get_priority` expose the fixed `0..7` policy. A priority
change is transactional, clears accumulated age, and is accepted only for a
constructing, ready, running, or blocked task. Task queries report the base
priority; exact event/epoch, join target/completion, deadline and cancellation
metadata; and safe stack-diagnostic availability.

## Join, deferred cancellation, and timed sleep

`scheduler_task_join` rejects bootstrap, self-joins, and every bounded
dependency cycle. A waiter either observes an already-complete target or enters
the fixed join waiter mask. Completion is copied into the waiter before it is
made ready. The waiter consumes that cached `{index,generation}` immediately on
resume, so reaping and reusing the target descriptor cannot turn a completed
join into a stale-handle failure.

Cancellation is deferred. `scheduler_task_request_cancel` is idempotent and
wakes an event-blocked, join-blocked, or sleeping target exactly once, while
removing its old waiter membership. The target exits only through
`scheduler_cancellation_point`; ordinary code is never asynchronously torn
down. Yield, waits, sleeps, join, cancellation exit, and ordinary task exit all
reject a caller pinned by an outer preemption guard or lock before publishing a
pending switch.

`scheduler_sleep_until` and `scheduler_sleep_for` use a monotonic `uint64_t`
tick domain. Past or zero-duration waits are rejected, relative overflow is
reported, and exhaustion at `UINT64_MAX` fails closed. Every timer interrupt
advances this domain and scans at most 16 descriptors even when the ready queue
is empty, so the last dynamic sleeper cannot be stranded. Woken tasks retain
normal priority/ring ordering.

## Bounded epoch events

Events are fixed integer identities `0..7`; they allocate no memory. A caller
first snapshots an event with `scheduler_event_observe`. A dynamic task passes
that token to `scheduler_event_wait`. The scheduler compares the token epoch
inside the same IF-disabled decision that would publish the blocked state. If a
wake occurred after observation, the wait returns
`SCHEDULER_STATUS_EVENT_CHANGED` without blocking, closing the lost-wakeup
window. The bootstrap task cannot block.

`scheduler_event_wake_one` wakes the lowest descriptor index in the event's
bounded 16-bit waiter mask; `scheduler_event_wake_all` wakes every waiter. Each
wake operation advances the nonzero 64-bit event epoch even when no task is
waiting. Epoch `UINT64_MAX` fails closed instead of wrapping. Wake-one updates
the epochs of waiters that remain blocked, so they remain owned by exactly the
new event generation. A blocked task keeps all 16 mapped stack frames and its
saved continuation, cannot be reaped, and becomes ready exactly once when
woken.

## AMD64 trap-frame contract

In 64-bit mode, hardware always pushes the complete `SS`, `RSP`, `RFLAGS`,
`CS`, and `RIP` return frame, including for a same-CPL interrupt, and `iretq`
always pops the `SS:RSP` pair. A hardware error code, when present, follows that
same shape. Zenith's stubs normalize the vector and error slots, and the common
entry prepends CR2 and every non-RSP GPR. The result is one 184-byte public ABI;
all 184 bytes are part of every ring-zero task's resumable image and must remain
inside its proven stack bounds.

A new stack is zeroed and observed zero in full. Its lowest 64 bytes then
receive a fixed canary and the remaining payload receives the deterministic
high-water sentinel before one synthetic 184-byte frame is built at the top.
Its RIP is the first-entry trampoline, CS is the kernel code selector,
RFLAGS contains the required reserved bit with IF initially clear, and vector
provenance is `0x31`. The trampoline enters C at the SysV alignment, enables IF
only after validating current ownership, and routes return through the
non-returning exit path. A saved frame is accepted only inside the exact owned
task payload (or linker-owned bootstrap stack), with aligned canonical address,
kernel CS/RIP, legal RFLAGS, live generation, and runnable-state provenance.
Task queries expose the resume-time RSP stored in that frame, not the frame's
own address; tasks without a saved continuation report zero.

The timer register probe arms only after every non-RSP GPR and DF contain their
sentinel values. The observer can acknowledge the probe only after a real
CPU-bound preemption, and both success and failure disarm it. The probe proves
DF and IF preservation. Return-frame validation also accepts the ordinary
status flags plus DF, IF, AC, and ID, while rejecting TF and RF so a resumed task
cannot single-step or suppress a pending instruction breakpoint. Because x86
normally sets RF in an exception's stacked RFLAGS image, the scheduler removes
that entry-only bit before validating or publishing the frame as a resumable
continuation; it never becomes saved task state.

The runtime enables CR0.MP/NE, clears EM/TS, enables CR4.OSXSAVE, derives a
standard-format bounded layout from CPUID leaf `0xD`, and enables only bounded
x87/SSE/AVX components present in XCR0. The image is at most 4,096 bytes and
64-byte aligned. Every frame switch eagerly saves the outgoing image before
restoring the incoming image while IF is clear. Image headers and MXCSR are
validated before XRSTOR. Unexpected `#NM` is fatal and poisons xstate; NMI and
IST paths remain xstate-free. Compiler-emitted MMX/SSE and autovectorization
remain prohibited, with only exact audited assembly probes and xstate helpers
allowed by disassembly inspection.

For a non-running dynamic task, `scheduler_task_stack_high_water` scans upward
from the intact bottom canary until the first changed sentinel. The bounded
frame-pointer unwinder follows at most 32 monotonic in-stack RBP records and
accepts only linked kernel-text return PCs. Running-task scans are rejected:
the current mutable stack is never scanned from code executing on that stack.
Bootstrap diagnostics are deliberately separate from dynamic-handle APIs. The
build requires frame pointers and disables sibling-call optimization.

The canonical GCC stack-usage pass enforces an 8,192-byte per-function ceiling
and requires the interrupt, kernel-main, create, first-entry, reap, worker, and
yield paths to be present. The 64 KiB payload
leaves substantial headroom beyond that ceiling for nested scheduler, heap, VM,
and test calls plus the normalized 184-byte interrupt frame. This is a bound,
not overflow recovery: x86 ring-zero interrupts arriving near the bottom of a
task payload do not automatically switch to a general guard stack. The adjacent
absent guard makes an overrun fault when the next access crosses the page
boundary, but cannot make arbitrary prior corruption safe.

## Interrupt state and timer interaction

Creation, priority, event, lifecycle, diagnostic, yield, exit, and reap
operations reject interrupt,
exception, NMI, and panic context through the existing dispatcher/panic
ownership signal. The timer handler only advances time and requests a
reschedule; the internal switch hook runs after any required EOI at interrupt
exit. Existing IST ownership for NMI, machine check, and double fault is
unchanged and those paths never switch, as is the 184-byte public
interrupt-frame ABI.

Every switch decision and frame handoff runs with `RFLAGS.IF=0`.
The suspended `scheduler_yield` or `scheduler_event_wait` frame retains its own
pre-call IF state and restores that exact state when it resumes. A
no-peer, changed-event, or validation return also restores the caller's state.
A newly started task enters C with IF enabled by documented policy. The
scheduler registers lock index 1 in `SPINLOCK_CLASS_SCHEDULER`. Complete public
metadata transactions hold that irqsave lock. The global order is TIME before
SCHEDULER before HEAP/VM/physical; the time lock is released before calling
`scheduler_timer_tick`. A voluntary path publishes its pending request under
the scheduler lock, releases the lock while IF remains clear, and only then
enters vector `0x31`. Interrupt exit reacquires the lock for candidate
publication, eager xstate transfer, and saved-frame ownership changes, then
releases it before `iretq` can run the selected task. This is a BSP runtime;
per-CPU run queues and cross-CPU migration remain future work.
Public entry points reject interrupt, NMI/IST, and panic ownership before they
touch the scheduler lock, so an interrupt of a lock holder cannot recurse or
spin on the interrupted CPU's own lock.
Initialization, creation, rollback, reaping, explicit validation, and audited
statistics perform a complete heap/frame/VM audit. A successful resource-boundary
audit records nonzero physical-owner and VM-mapping mutation epochs plus their
exact task-stack counts. Timer transitions, voluntary switch entry and resume,
metadata-only mutations, and read-only task diagnostics use bounded local
validation: they check the complete scheduler core, runtime records, saved-frame
provenance, stack bounds and canaries, exact eager-xstate ownership, and require
both current O(1) epoch/count certificates to match the last full audit. Any
task-stack PTE or owner mutation changes its epoch before publication, so a
stale local certificate fails closed. This avoids exhaustive allocator and VM
walks in every 1 kHz handler or ordinary scheduler query, which can otherwise
starve an interrupted task under emulation. Eager xstate also validates itself
at each actual frame handoff.

Ordinary tasks may call the heap outside scheduler critical and interrupt
context. The integrated proof keeps one allocation live across multiple yields,
checks its payload, frees it, and revalidates heap statistics.

## Creation, publication, and rollback

Creation is a candidate transaction:

1. Validate the active scheduler, ring, handles, VM hierarchy, guards, frame
   allocator, heap, mappings, and independent counts.
2. Validate the output pointer and require the entry address to be executable
   linked kernel text.
3. Reserve the lowest unused descriptor and validate its requested priority
   only in a candidate state image.
4. Acquire 16 task-owned frames, map each fixed leaf, and zero all 4 KiB before
   continuing.
5. After all 64 KiB are present and observed zero, install the bottom canary
   and high-water sentinel, then construct the synthetic kernel interrupt frame.
6. Revalidate bounds, frame provenance, 16 distinct exact mappings, guards,
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
selected ready frame becomes running. The exited frame is deliberately not
published as resumable before any reclamation is possible. The bootstrap task cannot use
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
publish, priority ordering and aging, epoch observation, block, wake-one,
wake-all, stale tokens, exit, reap, stale handles, generation exhaustion,
descriptor limits, reuse, counters, and injected corruption without importing
the C implementation's queue algorithm. Kernel tests add real frames, mappings,
IF behavior, priority ordering, block/wake/resume, blocked reap rejection,
assembly register/RFLAGS preservation, CPU-bound preemption, stack patterns,
heap use, timer progress, join completion retention, deferred cancellation,
empty-queue sleep wakeup, canary/high-water/unwind behavior,
rollback at every page, and descriptor exhaustion.

CI also runs 20 scheduler guests and 20 guard guests in balanced competing
two-way real-time TCG batches. Isolated QEMU scenarios retain their strict
15-second deadline. Competing guests have an explicit 30-second hard ceiling
so host scheduling contention is not mistaken for a kernel hang; the harness
allows no retries and still requires the exact marker protocol, diagnostics,
and debug-exit status for every guest.

The scheduler QEMU scenarios are `scheduler`, `scheduler-guard`, and
`scheduler-nm`. They require exactly one matching BEGIN/PASS pair each. The
main scenario additionally emits:

```text
ZT BEGIN scheduler
ZT PROOF scheduler-eager-xstate
ZT PROOF scheduler-stack-diagnostics
ZT PROOF scheduler-lifecycle
ZT PASS scheduler

ZT BEGIN scheduler-guard
ZT PASS scheduler-guard

ZT BEGIN scheduler-nm
ZT PASS scheduler-nm
```

The scheduler scenario writes debug-exit value `0x1D`, observed by the host as
59. It runs three tasks through a deterministic trace, covers explicit and
return exit, reaping and stale handles, slot reuse, 16-descriptor exhaustion,
frame OOM, all creation map failures, uncertain mapping cleanup, context
rejection, priority ordering, epoch-event block/wake/resume, required
all GPRs, DF/IF, task-local data, heap persistence, timer/EOI progress, and a
CPU-bound task that never calls yield but is preempted so an observer can run.

The guard scenario selects slot zero's lower guard at
`0xFFFFA00000000000`, proves it is absent, then performs an assembly load. It
requires a supervisor non-present page fault (`P=0 W=0 U=0 RSVD=0 I=0`), exact
CR2, debug-exit value `0x1E` observed as 61, and one matching marker pair.
The `scheduler-nm` scenario initializes scheduler/xstate, sets CR0.TS in an
assembly-only probe, and executes one audited XMM instruction. It requires
vector 7 at the exact fault site, `xstate=unexpected device-not-available`,
xstate poisoning, and debug-exit value `0x1F` observed as 63.
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

The scheduler has no userspace execution, privilege-transition stack, SMP run
queue, load balancing, task migration, dynamic VM allocation, or asynchronous
cancellation. Stack canaries detect but cannot recover corruption, high-water
measurements are historical rather than a hard proof of future headroom, and
the frame-pointer unwinder intentionally refuses the current mutable stack.
TSS.RSP0 remains a future privilege-transition
concern. Storage, networking, graphics, and a general driver framework are also
outside this milestone.

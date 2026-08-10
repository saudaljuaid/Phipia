# Bounded kernel heap

This milestone adds one deliberately narrow dynamic-memory consumer. It extends
the existing physical-frame allocator and permanent 4 KiB hierarchy; it does
not introduce a second frame allocator or claim to be a general virtual-memory
manager.

## Virtual window and table bound

The payload window is the canonical, page-aligned half-open range
`[0xFFFF900000000000, 0xFFFF900001000000)`, exactly 16 MiB or 4096 pages. The
page at `0xFFFF8FFFFFFFF000` is the lower guard and the page at
`0xFFFF900001000000` is the upper guard. Both must remain absent.

The window is disjoint from the low identity-mapped kernel and its static page
tables, the VGA page, the `0xFFFF800000000000` APIC device window, and all
linked sections. Boot-time layout validation proves canonicality, alignment,
non-wrapping endpoints, and those non-overlap rules before CR3 changes.

The permanent page-table arena remains 64 pages, or 256 KiB of BSS. Preparing
the heap consumes exactly ten additional arena pages in the current layout: one
PDPT, one page directory, and eight leaf tables. All parent and leaf tables are
created before the permanent CR3 is published. Runtime growth therefore
changes leaf PTEs only and cannot allocate another table. A pure builder test
proves that a ten-page total arena is insufficient and that eleven pages
including the root are sufficient for an otherwise empty hierarchy. The
existing 64-page bound retains headroom for the linked kernel and APIC maps; it
has not been enlarged or made dynamic.

## Mapping invariant

Every present heap leaf is supervisor-only, writable, non-executable, and
ordinary write-back memory. Its only software-selected leaf bits are P, RW, and
NX; the processor may additionally set A and D. US, PWT, PCD, PAT, global, and
huge-page bits are absent. CR0.WP, EFER.NXE, the architectural PAT layout, W^X,
owned parent-table addresses, both guard holes, and the exact count of present
heap leaves are revalidated.

The runtime mapping boundary accepts one aligned page inside the heap window
and one physical frame within the CPUID physical-width mask. It rejects missing
or conflicting parents, a nonzero absent leaf, an existing mapping, and every
bad address before mutation. Unmap requires the caller to provide the exact
physical frame and exact heap permissions. A successful leaf mutation is
followed by a compiler memory barrier and `invlpg` for that virtual page. A
failed post-mutation validation restores the old leaf, executes `invlpg` again,
and reports whether restoration itself failed.

This map/query/unmap surface is intentionally heap-specific. It is not a
general mapper, it cannot select arbitrary permissions, and it does not hand
runtime table ownership to other consumers.

## Allocator and metadata ownership

Allocator metadata never resides in caller payloads. Three fixed BSS state
images (active, transactional candidate, and pure-test state), two 4096-entry
frame arrays, and one mapping-state array add about 152.3 KiB of fixed BSS.
There is no metadata allocation and therefore no heap recursion.

The physical allocator separately spends fixed 128 KiB owner bitmaps for the
complete below-4-GiB frame domain. The heap bitmap gives every heap frame an
identity distinct from generic and task-stack ownership. The heap neither
duplicates that state nor infers ownership from global allocation counts.

The active state contains at most 512 descriptors. Its linked descriptors must
cover `[0, committed_bytes)` exactly once, in ascending order, without gaps,
overlap, zero-length blocks, cycles, unlinked active descriptors, or adjacent
free blocks. Every descriptor carries an integrity value over its index and all
trusted fields. Full validation checks the magic and inverse magic first, then
every descriptor and link, then independently recomputes allocation and byte
statistics. Integrity values are corruption detectors, not cryptographic
authentication.

Allocation is bounded first-fit. The public alignment must be a power of two in
`[1, 4096]`; returned addresses use `max(requested_alignment, 16)`, so every
allocation is at least 16-byte aligned. A zero byte request is invalid. The
allocator checks every addition, alignment round-up, page round-up, capacity
comparison, and virtual-address calculation. A split uses zero, one, or two of
the lowest-numbered unused descriptors and is published only if all required
descriptors exist. Every list walk is capped at 512 iterations.

Free first validates the complete allocator, then checks the numerical pointer
range and 16-byte alignment before using the offset as provenance. Only the
exact start of a live descriptor can be freed. Live interior, outside,
misaligned, already-free, and impossible pointers receive distinct deterministic
rejections where the state permits that distinction. Free blocks coalesce with
both neighbors immediately and deterministically. Payloads are not poisoned in
this milestone; validity never depends on payload contents.

External fragmentation is possible because allocation is first-fit and live
blocks are not moved. Descriptor exhaustion can occur before byte exhaustion
under adversarial fragmentation. Both conditions return without changing the
active state. Splitting and coalescing are non-recursive and bounded by the
fixed descriptor limit.

## Growth transaction and rollback

Growth follows this ownership sequence:

1. Validate the active heap, frame allocator, permanent hierarchy, guards,
   mappings, and exact heap-owner frame count.
2. Copy metadata into the fixed candidate state, calculate checked page-rounded
   growth, grow the candidate, and satisfy the request there.
3. Obtain each frame from the existing frame allocator with the kernel-heap
   owner identity and map it at the next committed heap page through the
   runtime leaf API.
4. Zero all 4096 bytes of each newly mapped frame before any allocation pointer
   or candidate metadata is published.
5. Validate the candidate, every old and new frame-to-leaf association, exact
   mapping and heap-owner counts, permissions, guards, CR0.WP, EFER.NXE, PAT,
   and the complete hierarchy. Unrelated generic allocations do not weaken or
   invalidate this check.
6. Transfer the new frame ownership records, publish the candidate metadata,
   and only then return the pointer.

If any frame acquisition, mapping, or later validation fails, mapped pages are
unmapped in reverse order and each successfully unmapped or never-mapped frame
is released. Every unmap invalidates its TLB entry. Active metadata remains
unchanged. Rollback then revalidates the frame and mapping counts. A frame is
never released while its mapping may still exist. If an unmap, release, or
rollback validation fails, the heap is permanently poisoned and reports
`HEAP_STATUS_ROLLBACK_FAILURE`; subsequent operations fail rather than risk
double ownership or silent reuse.

Completely free pages are **not returned** to the frame allocator in this
milestone. Freed bytes immediately become reusable by first-fit, but all
committed backing frames remain owned and mapped by the heap until reboot. This
is observable in bounded statistics and is not described as physical-memory
reclamation.

## Interrupt-state contract

The kernel is single-core and cooperatively scheduled without preemption; its
periodic Local APIC timer remains active but never selects a task. Heap entry is
allowed from ordinary task context and forbidden while the C interrupt
dispatcher is active, including exception and NMI handlers, and after panic has
started. Heap functions must not be called from assembly-only interrupt entry,
panic, or other emergency paths either.

Ordinary metadata work and frame zeroing do not mask interrupts. Each leaf
mutation uses one bounded IF-disabled section because the virtual-memory API
requires it. The wrapper records IF before `cli` and executes `sti` only when IF
was originally set; a caller that entered with IF clear returns with IF clear.
No spinlock, wait, recursion, or SMP-safety claim is present. The integrated
scenario checks both IF-preservation states and proves monotonic ticks and APIC
EOIs continue across normal heap work.

## API and deterministic failure

`include/zenith/heap.h` exposes only initialization, allocation with byte size
and alignment plus an output pointer, deallocation, validation, bounded
statistics, pure self-test, and status strings. There is no `malloc`, `calloc`,
`realloc`, C++ allocation, constructor, or compatibility alias.

Expected resource and rejection failures return a specific status and never
publish a pointer. Metadata corruption, impossible statistics, and failed
rollback also fail closed. The worst credible failure is a corrupt block or PTE
creating overlap, W+X memory, a stale alias or TLB entry, leaked or doubly owned
frames, or damage outside the heap. The design addresses that risk with
external metadata, checked arithmetic, complete bounded validation, candidate
publication, exact frame provenance, reverse rollback, `invlpg`, guard holes,
and permanent poisoning when safe recovery cannot be demonstrated.

## Executable protocol

Pure kernel tests cover size, alignment, arithmetic and page-rounding edges,
exact fit, aligned splitting, forward and backward coalescing, reuse, pointer
rejection, statistics, table exhaustion, mapping conflict, canonicality,
physical-width rejection, and metadata corruption. The host runner is compared
with an independent Python list-model oracle for 100,000 deterministic
randomized operations.

The heap scenario remains the thirteenth QEMU scenario. It must emit exactly:

```text
ZT BEGIN heap
ZT PASS heap
```

and write debug-exit value `0x1C`, observed by the host as status 57. For a
three-page growth it injects failure before the first, second, and third frame
acquisition and PTE map, proving complete rollback from every transaction step.
It separately injects an uncertain post-write mapper result, then proves the
outer transaction unmaps the leaf before releasing its heap-owned frame.
It also invokes the public validation API from a software-interrupt handler and
requires context rejection. Before heap execution, the scenario deliberately
discards one otherwise valid APIC calibration attempt and proves the bounded
retry still activates the monotonic clock. Real allocations below, equal to,
and above a page
exercise power-of-two alignment, non-overlap, payload persistence, reuse,
bidirectional coalescing, invalid and double frees, and whole-window exhaustion.
It queries RW+NX supervisor leaves and both guards, checks exact frame and heap
statistics, preserves the masked PIT route, and verifies the Local APIC
monotonic clock still advances. Normal boot emits:

```text
Zenith OS: bounded kernel heap verified
```

The heap scenario additionally emits
`Zenith OS: APIC calibration retry verified` after the injected discard has
been consumed and the Local APIC timer has passed full activation validation.

The default host matrix uses 128 MiB of guest RAM. Nineteen MiB remains the
lowest supported whole-MiB configuration for the complete protocol after the
guarded-stack milestone. The canonical GCC/BIOS build reports 4,266 allocatable
frames there. Eighteen MiB exposes 4,010, fewer than the 4,096 simultaneous heap
frames the scenario deliberately requires, and therefore exercises physical OOM
rather than the required virtual-window boundary.

## Residual risk and deferred work

Testing cannot prove the absence of every allocator or MMU defect. Descriptor
integrity is not protection against deliberate arbitrary writes. A stale raw
pointer whose address has since been reallocated cannot be distinguished from
the new owner's valid pointer. Explicit frame-owner tags prevent generic/heap
cross-release and make heap validation independent of generic allocations, but they
cannot protect against an arbitrary write that corrupts both allocator state
and its validation inputs. First-fit can fragment, free pages do not shrink, and
the 512-descriptor ceiling is intentionally small. There is no sanitizing red
zone inside the payload window, use-after-free detector, SMP coordination,
lock, per-CPU heap, interrupt-context pool, general mapping consumer, dynamic
page-table allocation, process address space, userspace, preemption, blocking
synchronization, storage, networking, or driver framework. The cooperative
scheduler's separate ownership contract is documented in
`docs/KERNEL_SCHEDULER.md`. The remaining features require their own ownership
and executable acceptance contracts.

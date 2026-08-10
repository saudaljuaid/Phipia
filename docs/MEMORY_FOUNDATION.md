# Memory foundation

This increment turns the boot loader's byte buffer into a validated Zenith
contract and establishes ownership of physical frames below 4 GiB.

## Trust boundary

Multiboot2 data is external input. The parser rejects a bad magic value, null or
misaligned information address, unreasonable total size, arithmetic overflow,
nonzero reserved header field, truncated or undersized tags, duplicate memory
maps, malformed entry geometry, unterminated strings, and a missing or misplaced
end tag. Unknown tags are skipped only after their bounds have been proven.
Boot modules are rejected until Zenith can assign their lifetime explicitly;
silently treating module storage as allocatable memory would corrupt them.

The parser never follows a tag pointer outside the declared information block.
The early assembly map covers the complete 32-bit physical-address range so a
valid Multiboot2 pointer remains accessible while the permanent virtual-memory
layout is still under construction.

## Frame ownership

The frame allocator tracks 4 KiB frames below 4 GiB with fixed eligibility,
use, heap-owner, and task-stack-owner bitmaps. Each bitmap costs 128 KiB of
BSS. A frame becomes eligible only when the firmware map calls the entire page
available. A second pass lets every reserved or bad region override an
overlapping available entry.

Zenith permanently removes these ranges from allocation:

- the first 1 MiB, including firmware data and VGA memory;
- the complete linked kernel image, including all four allocator bitmaps;
- the live Multiboot2 information block;
- every non-available or unreported physical range.

Allocation is deterministic first-fit with a rotating search hint. Every live
allocatable frame has exactly one explicit owner: generic kernel, kernel heap,
or task stack. Release requires the matching owner and rejects unaligned
addresses, addresses beyond the early map, permanent ranges, owner mismatches,
and double frees.
Allocate, release, and reserve reject impossible cached counters before any
bitmap or statistic mutation. Range reservation refuses to steal an allocated
frame. A bounded validation
pass independently reconstructs generic, heap, task-stack, total, free, and
reserved counts; it also rejects an owner bit on any free or ineligible frame.

The kernel heap is the first owned runtime consumer. Heap growth requests the
heap identity, records one exact frame for each committed virtual page, and
requires the allocator's heap-owned count to equal the mapped-page count.
Generic consumers may allocate later without invalidating that proof. Failed
growth releases every frame with the heap identity only after its mapping was
removed; a generic release cannot free it. Completely free heap pages do not
return here in this milestone, so heap-owned statistics continue to include all
committed heap pages.

## Executable acceptance test

During every QEMU boot the kernel parses the real GRUB memory map, initializes
the allocator, obtains two distinct aligned generic frames, rejects a
heap-owner release, completes one explicit heap-owner allocate/query/release
lifecycle, completes the equivalent task-stack-owner lifecycle with generic and
heap cross-release rejection, releases the generic frames in reverse order, and
proves all three owner counts returned to zero. CI accepts the increment only
after COM1 emits:

```text
Zenith OS: memory foundation passed
```

This is still an early single-core allocator. Heap callers are excluded from
interrupt, exception, NMI, and panic context, and runtime PTE changes preserve
the caller's IF state. NUMA policy, dynamic page-table allocation, memory above
4 GiB, process address spaces, and SMP synchronization are explicitly deferred.
The permanent hierarchy owns static tables inside the kernel image and adds
only bounded heap-specific and task-stack-specific runtime leaf mapping
surfaces.

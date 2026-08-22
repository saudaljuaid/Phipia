# Memory foundation

This increment turns the boot loader's byte buffer into a validated Sapote
contract and establishes ownership of physical frames below 4 GiB.

## Trust boundary

Multiboot2 data is external input. The parser rejects a bad magic value, null or
misaligned information address, unreasonable total size, arithmetic overflow,
nonzero reserved header field, truncated or undersized tags, duplicate memory
maps, malformed entry geometry, unterminated strings, and a missing or misplaced
end tag. Unknown tags are skipped only after their bounds have been proven.
Boot modules are rejected until Sapote can assign their lifetime explicitly;
silently treating module storage as allocatable memory would corrupt them.

The parser never follows a tag pointer outside the declared information block.
The early assembly map covers the complete 32-bit physical-address range so a
valid Multiboot2 pointer remains accessible while the permanent virtual-memory
layout is still under construction.

## Frame ownership

The frame allocator tracks 4 KiB frames below 4 GiB with separate eligibility
and use bitmaps. A frame becomes eligible only when the firmware map calls the
entire page available. A second pass lets every reserved or bad region override
an overlapping available entry.

Sapote permanently removes these ranges from allocation:

- the first 1 MiB, including firmware data and VGA memory;
- the complete linked kernel image, including both allocator bitmaps;
- the live Multiboot2 information block;
- every non-available or unreported physical range.

Allocation is deterministic first-fit with a rotating search hint. Release
rejects unaligned addresses, addresses beyond the early map, permanent ranges,
and double frees. Range reservation refuses to steal an allocated frame.

The same bitmap now backs bounded contiguous allocations. Requests name a page
count, page-sized power-of-two alignment, and inclusive maximum address; typed
generation handles make release explicit and prevent individual frames from
being removed from a live extent. `docs/DMA.md` records the checked arithmetic,
ownership states, exhaustion and wrong-owner controls used by devices.

## Executable acceptance test

During every QEMU boot the kernel parses the real GRUB memory map, initializes
the allocator, obtains two distinct aligned frames, releases them in reverse
order, and proves the allocated-frame count returned to zero. CI accepts the
increment only after COM1 emits:

```text
Sapote: memory foundation passed
```

This is still an early single-core allocator. Interrupt safety, synchronization,
NUMA policy, memory above 4 GiB, IOMMU isolation, page-table ownership, and virtual mappings are
explicitly deferred.

## v0.4.0 xHCI consumer

The xHCI proof uses at most eight existing contiguous-DMA records: an
administration page, three rings, two context pages, the receive page, and one
optional multi-page scratchpad allocation. It adds no allocator path or hidden
physical mapping. All requests are bounded below 4 GiB, and reverse teardown
returns the frame/contiguous-allocation census to its entry snapshot. See
`docs/XHCI_HOST.md`.

## v0.5.0 NVMe consumer

The NVMe proof obtains nine contiguous pages through seven existing DMA
records: six one-page queues/Identify buffers and one three-page guarded Read
allocation. It adds no allocator path or hidden physical mapping. The middle
Read page is PRP1; its neighbours remain sentinels. Full reverse teardown must
restore both the frame and contiguous-allocation census to the entry snapshot.
See `docs/NVME_CONTROLLER.md` and `docs/BLOCK_READ.md`.

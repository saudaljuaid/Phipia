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

The frame allocator tracks 4 KiB frames below 4 GiB with separate eligibility
and use bitmaps. A frame becomes eligible only when the firmware map calls the
entire page available. A second pass lets every reserved or bad region override
an overlapping available entry.

Zenith permanently removes these ranges from allocation:

- the first 1 MiB, including firmware data and VGA memory;
- the complete linked kernel image, including both allocator bitmaps;
- the live Multiboot2 information block;
- every non-available or unreported physical range.

Allocation is deterministic first-fit with a rotating search hint. Release
rejects unaligned addresses, addresses beyond the early map, permanent ranges,
and double frees. Range reservation refuses to steal an allocated frame.

## Executable acceptance test

During every QEMU boot the kernel parses the real GRUB memory map, initializes
the allocator, obtains two distinct aligned frames, releases them in reverse
order, and proves the allocated-frame count returned to zero. CI accepts the
increment only after COM1 emits:

```text
Zenith OS: memory foundation passed
```

This is still an early single-core allocator. Interrupt safety, NUMA policy,
dynamic page-table allocation, memory above 4 GiB, process address spaces, and
SMP synchronization are explicitly deferred. The permanent boot hierarchy now
owns static tables inside the kernel image and maps only the kernel, VGA, and
discovered APIC device pages.

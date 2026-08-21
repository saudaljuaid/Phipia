# Bounded DMA memory

The physical-frame allocator now owns a fixed table of contiguous allocations.
A `struct frame_contiguous_request` names page count, power-of-two byte
alignment, and maximum physical address inclusive. The returned typed handle
records the exact base, count, alignment, bound, generation identifier, and
active state. Checked multiplication and range arithmetic precede a bounded
bitmap scan; every page must be allocatable and free.

Zero pages, sub-page or non-power-of-two alignment, alignment that cannot fit
below the bound, an unsatisfied address bound, exhaustion, record-table
exhaustion, stale handles, ordinary single-page release of a live contiguous
range, and double release are refused. Release verifies the generation, base,
count, eligibility, and allocated bits before changing any bitmap state.

## DMA ownership

`struct dma_allocation` adds a CPU address, byte length, initialization bit, and
an explicit owner. New allocations are CPU-owned identity-mapped write-back
memory. A driver initializes descriptors and buffers, marks them prepared, and
then transfers ownership to the device. CPU-to-device transfer before
initialization, repeated transfers, CPU access claimed while device-owned, and
release from device ownership are rejected. Completion transfers ownership
back to the CPU before validation or release.

The public handle is evidence, not authority by itself. A private fixed record
binds its address, frame-generation identifier, physical range, initialization
state, and owner to the exact live handle. Every lifecycle operation and the
bus-master predicate compare against that record, so copying or fabricating the
public fields cannot manufacture device ownership.

PCI bus mastering is gated by this state machine. It may be enabled only for a
live claim with mapped memory resources and a bounded list of initialized,
device-owned DMA allocations. It must be disabled before ownership is reclaimed
or memory is released. Compiler ordering is paired with the x86 store ordering
used before a device notification.

## Security boundary

Sapote has no IOMMU. Address bounds and software ownership prevent accidental
driver misuse and make teardown auditable; they do **not** isolate a malicious
or defective bus-mastering device. Such a device can DMA to physical memory
outside the allocations given to it. Production isolation requires an IOMMU
and interrupt remapping.

## Executed controls

The DMA boot stage exercises zero-page, malformed and impossible alignment,
transfer-before-initialization, repeated and wrong-owner transfers,
device-owned release, a forged copied handle, valid round-trip, and double
release paths. The VirtIO RNG
proof additionally bounds both pages below 4 GiB, refuses premature bus
mastering, observes CPU → device → CPU ownership, and requires all allocation
counts to return to zero.

## Deferred work

There is no scatter/gather list, streaming-map API, cache maintenance for
noncoherent machines, bounce buffering, IOMMU domain, or user mapping. The API
is intentionally sufficient for first PCI drivers without pretending to offer
device isolation.

## xHCI object and TRB ownership

v0.4.0 applies the existing allocation owner to administration, command, event,
control, context, receive, and optional scratchpad allocations. Live rings also
track producer/consumer TRB ownership: a CPU-owned producer slot may be
initialized and published, while a controller-owned event may be inspected only
after the interrupt path returns the event allocation to the CPU. The receive
page starts with a sentinel, becomes controller-owned before the endpoint-zero
doorbell, and returns to CPU ownership only on its exact matching transfer
event. Teardown first proves the controller halted and disables PCI bus
mastering; only then may it reclaim or release DMA. This does not change the
no-IOMMU security boundary.

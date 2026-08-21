# PCI resource ownership

PCI enumeration identifies functions; it does not authorize a driver to touch
their address space. `pci_resource.c` adds that missing ownership boundary.
Every operation takes a bounded `struct pci_device_claim`,
`struct pci_bar_description`, or `struct pci_mmio_region`. A caller cannot map a
loose physical address and later present it as a BAR.

The implementation follows the current PCI Express Base Specification's BAR
and command-register rules. The public specification landing page is
<https://pcisig.com/specification-overview/pci-express-base>.

## Claim transaction

A claim is accepted only for a function returned by the installed PCI
enumeration and only while interrupts are disabled. The kernel snapshots the
command register and every BAR, clears I/O decode, memory decode, and bus
mastering, reads the command register back to prove decode is actually off,
writes all ones to each BAR, reads the sizing mask, and restores all
BARs and the command register exactly. Type 0 headers expose six BAR slots and
type 1 headers expose two. Unimplemented, I/O, 32-bit memory, and paired 64-bit
memory BARs are represented explicitly; the low attribute bits are retained in
the description and platform addresses are never reassigned.

Sizes must be nonzero powers of two. Bases must be assigned, naturally aligned,
and have a checked inclusive end. A 64-bit low half must have a following slot.
Resources of the same address-space kind may not overlap within a function or
across live claims. Any probe or validation failure restores the saved values
before returning a named status. A successful claim preserves the platform's
decode state but always leaves bus mastering disabled.

## Device-MMIO arena

Claimed memory BARs are mapped into the 64 MiB arena beginning at virtual
`0x0000000C00000000` (48 GiB). This lies above the kernel identity region,
paging probes, 16 GiB heap, and 32 GiB thread-stack arena and below the
canonical-address boundary. The allocator rounds physical spans with checked
arithmetic and tracks each 4 KiB virtual page in a fixed bitmap.

Before installing a mapping, the rounded physical span is checked against the
frame allocator's usable-memory bitmap. A BAR that aliases allocator-managed
RAM is refused by name, preventing a writable UC alias of ordinary WB memory.

Mappings use the paging layer's supervisor-only, writable, uncacheable policy.
They are non-executable. Translation is read back before the mapping becomes
owned. Subregions must fit the already-sized BAR, and teardown is last-mapped,
first-unmapped so rollback is deterministic. Releasing a claim disables bus
mastering, unmaps every BAR in reverse order, restores the original command
register, and invalidates the claim identifier.

## Configuration writes and bus mastering

Checked 8-, 16-, and 32-bit configuration writes exist for mechanism #1 and
for the mapped ECAM window. They reject nonzero mechanism-#1 segments, bad
device/function fields, misalignment, unsupported widths, offsets outside 256
bytes for ports or 4096 bytes for ECAM, and accesses outside the mapped bus
window. They require interrupts disabled so a configuration-address selection
cannot be interleaved.

Bus mastering is a separate claim transition. It is refused until at least one
memory BAR is mapped and every bounded DMA allocation supplied by the caller is
initialized and device-owned. Drivers must disable it before reclaiming DMA.

## Executed controls

Every boot disables decode before the real probe and injects one failure after
the first BAR write; it then reads back the command register and every BAR.
Pure controls reject a probe attempted with decode enabled, a 64-bit BAR in the
last slot, an overflowing range, overlapping ranges, and a device mapping that
aliases allocator-managed RAM. The VirtIO proof also
attempts to enable bus mastering before its queue and receive buffer change to
device ownership. Closing proofs require zero live claims, mappings, arena
pages, and bus masters.

## Deferred work

This layer does not reassign BARs, arbitrate bridge windows, implement SR-IOV,
or provide hotplug. It is the resource substrate for later drivers, not a
driver model.

## xHCI claim consumer

v0.4.0's xHCI host keeps one typed claim across BAR validation, reset, DMA
preparation, MSI-X binding, operation, slot disable, and teardown. Bus mastering
is enabled only after the complete seven-or-eight-allocation request is
initialized and device-owned. Halt is confirmed and bus mastering is disabled
before any allocation returns to the CPU. Every partial failure releases the
same claim and mapping in reverse order; the final resource snapshot must equal
the entry snapshot.

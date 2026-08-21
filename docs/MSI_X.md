# Dynamic vectors and MSI-X

Sapote now allocates interrupt vectors instead of assigning new devices fixed
numbers. Exceptions `0x00`-`0x1F`, PIC `0x20`-`0x2F`, I/O APIC `0x30`-`0x3F`,
local-APIC `0x40`-`0x47`, the unexpected-vector proof at `0x80`, the IST proof
at `0xF0`, and the spurious vector at `0xFF` are compile-time audited. The fixed
allocatable interval is `0x90` through `0xEF` inclusive.

`struct interrupt_vector_allocation` carries a vector, generation, and active
state. Allocation, exact allocation, exhaustion, reserved requests, stale
handles, and double release have distinct results. The boot control requests
reserved vectors, exhausts the complete interval, proves one more allocation is
refused, releases every handle, and proves a second release is refused.

## MSI-X binding transaction

`msix_bind` accepts an owned PCI claim, table entry, typed handler, and context.
It validates the capability identifier and bounded structure, table size,
entry, BIRs, table offset, and PBA offset. The entire table and pending-bit array
must fit the BAR sizes discovered by the claim. Both are reachable only through
the claim's uncacheable, non-executable mappings.

The function-mask bit is set before mapping or programming. The selected
16-byte entry is saved and masked; then a dynamic vector is allocated and the
local APIC address (`0xFEE00000` plus the destination APIC ID) and fixed-delivery
vector data are written with volatile accesses and store ordering. The handler
is registered before either the table entry or function is unmasked. An
already-enabled MSI-X function is refused because its state is not owned by the
new binding.

Rollback is the inverse acquisition order: function mask, vector mask, handler
unregister, original table entry, vector release, PBA/table unmap, and original
capability control. The live VirtIO proof injects a failure after handler
registration and requires no remaining handler, vector, mapping, or binding
before it performs the real bind.

Dynamic MSI-X dispatch does not program the I/O APIC and never sends a directed
EOI. The common interrupt return path acknowledges the local APIC after the
registered handler returns.

The layout and ordering are based on PCI 3.0 §6.8.2 and were cross-checked
against the current PCI Express Base Specification publication (approved
revision 7.0), published from
<https://pcisig.com/specification-overview/pci-express-base>.

## Deferred work

There is no MSI binding, interrupt remapping, affinity policy, multi-CPU target
selection, or shared-vector fanout yet. One binding owns one table entry and one
handler on the bootstrap processor.

## xHCI interrupter zero

The v0.4.0 host binds MSI-X entry zero only after the event ring and handler are
ready and before xHCI interrupt enablement or bus mastering. Interrupter zero's
ERST/ERDP state is processed in the handler; a completion counts only when the
event identity matches the outstanding TRB. The shared dynamic dispatcher then
uses the normal local-APIC EOI. The descriptor scenario snapshots the bound
count immediately before its endpoint-zero doorbell and requires an exact `+1`
transition. It never programs an I/O APIC route or requests directed EOI.

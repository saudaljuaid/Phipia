# Bounded NVMe controller

Sapote v0.5.0 owns one PCIe NVMe controller long enough to identify one
namespace, create one I/O queue pair, read one logical block and tear every
resource down. This is a block-controller foundation. It is not a filesystem,
partition layer, general storage stack or write-capable driver.

## Normative basis

The implementation was written against these current official publications,
not reconstructed from remembered layouts:

- PCI Code and ID Assignment Specification 1.19, §1, mass-storage class
  `01h`, non-volatile-memory subclass `08h`, NVMe programming interface
  `02h`: <https://pcisig.com/PCIExpress/Spec/Base/CodeandIDAssignment_1.19>.
- NVM Express Base Specification 2.4, §§3.1.4 and 3.5.1 for `CAP`, `VS`, `CC`,
  `CSTS`, `AQA`, `ASQ`, `ACQ` and controller initialization; §§4.1.1,
  4.2.1, 4.2.3 and 4.2.4 for submission/completion entries, phase and status;
  §4.3.1 for PRPs; §5.2.14 for Identify; §§5.3.1 and 5.3.2 for Create I/O
  Completion Queue and Create I/O Submission Queue:
  <https://nvmexpress.org/wp-content/uploads/NVM-Express-Base-Specification-Revision-2.4-Ratified-2026.07.31.pdf>.
- NVM Express over PCIe Transport Specification 1.4, §§3.1.1 and 3.1.2 for
  native-width volatile MMIO, ordering and doorbells; §§3.2 and 3.5 for queue
  interrupt vectors and MSI-X:
  <https://nvmexpress.org/wp-content/uploads/NVM-Express-NVMe-over-PCIe-Transport-Specification-Revision-1.4-Ratified-2026.07.31.pdf>.
- NVM Command Set Specification 1.3, §3.3.4 for NVM Read and §4.1.5.1 for
  the Identify Namespace data structure:
  <https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-Revision-1.3-Ratified-2026.07.31.pdf>.

## Exact supported shape

| Object | Bound |
| --- | ---: |
| PCI functions/controllers | 1 |
| namespaces | namespace identifier 1 only |
| MSI-X entries/vectors | entry 0, one vector |
| Admin SQ/CQ | identifiers 0/0, depth 2 |
| I/O SQ/CQ | identifiers 1/1, depth 2 |
| outstanding commands | one per queue |
| Identify buffers | two 4 KiB allocations |
| read payload | one 4 KiB logical block in one PRP1 page |

The small queue depth is intentional. `CAP.MQES` must admit two entries;
`CAP.CSS` must admit the NVM command set; `CAP.MPSMIN` and `CAP.MPSMAX` must
admit Sapote's 4 KiB page; `CAP.DSTRD` must produce aligned, in-BAR doorbells;
and the advertised version must be NVMe 1.4 or a 2.x version. Checked
arithmetic computes every fixed-register and doorbell span before the first
controller-state change. The complete touched span must fit BAR 0 and the PCI
resource layer must return the matching uncacheable subregion.

`CAP.TO` is converted from its 500 ms units to a checked monotonic deadline.
Controller disable clears `CC.EN` and waits for `CSTS.RDY == 0`; controller
enable programs the supported command set, 4 KiB page, 64-byte SQ entries and
16-byte CQ entries, sets `CC.EN`, and waits for `CSTS.RDY == 1`. `CSTS.CFS`
always terminates a wait as a named controller-fatal result. All other writable
`CC` fields are cleared through the defined writable mask; reserved bits retain
their read value.

`CAP`, `ASQ` and `ACQ` use single native-width volatile 64-bit operations.
Other registers and doorbells use native 32-bit volatile operations. A CPU
store fence and compiler memory barrier publish DMA content before each
controller-visible register or doorbell write. This follows the PCIe transport
ordering requirements and Sapote's existing `cpu_store_fence` contract.

## State and ownership

The controller handle has the monotonic states `discovered`, `claimed`,
`disabled`, `prepared`, `running`, `stopping` and `released`. A repeated,
reversed or skipped transition has a distinct status. The acquisition order is:

1. discover exactly one PCI class `01/08/02` function;
2. claim it and map BAR 0 through the PCI-resource APIs;
3. validate `CAP`, `VS`, every fixed register and all four doorbells;
4. disable the controller and observe `RDY == 0`;
5. allocate and initialize Admin queues, both Identify buffers, reserved I/O
   queues and the guarded read allocation through the DMA API;
6. install one dynamic handler and bind MSI-X entry zero while both the entry
   and function remain masked;
7. transfer all seven prepared allocations to controller ownership;
8. prove premature bus-master enable is refused, then enable bus mastering;
9. program `AQA`, `ASQ`, `ACQ`, `CC`, observe `RDY == 1`, then unmask MSI-X;
10. execute Identify Controller, Identify Active Namespace ID List, Identify
    Namespace, Create I/O CQ, Create I/O SQ and one NVM Read.

The CPU owns a submission allocation while constructing its next entry. It
returns that allocation to controller ownership before ringing the SQ tail
doorbell. A completion allocation remains controller-owned until the MSI-X
handler masks delivery and transfers it to the CPU. Only then does the handler
read its phase, command identifier, submission-queue identifier, submission
head and status. It advances the CQ head, republishes the CQ to the controller,
rings the correct CQ doorbell and unmasks delivery before the common interrupt
dispatcher performs its normal local-APIC acknowledgement. There is no I/O APIC
route and no directed EOI.

The completion entry has no returned-byte-count field for these commands.
Sapote instead validates the command's exact transfer length against its DMA
allocation before submission, validates the CQ submission-head bound on
completion, and validates the returned Identify or logical-block data after
ownership returns to the CPU. An unexpected phase is not consumed. A matching
phase with the wrong CID, SQID or status is consumed, counted and reported, but
never satisfies the outstanding wait.

## Teardown

Teardown runs for every acquisition boundary. It disables the controller and
observes `RDY == 0`, masks MSI-X, disables PCI bus mastering, unbinds the
handler/vector/table mapping, transfers every device-owned allocation back to
the CPU, releases allocations in reverse order and finally releases the PCI
mapping and claim. Entry and exit snapshots require identical PCI claims,
mappings, mapped pages, bus masters, DMA owners/allocations, frame counts,
vectors and MSI-X bindings.

If bus mastering cannot be proven disabled, the routine does not reclaim DMA.
That failure is a safe refusal rather than use-after-free. The controlled race
hook detects any interrupt observation after completion storage becomes
unavailable and never dereferences freed state.

Sapote has no IOMMU. Typed bounds and ownership prevent the kernel from
misusing a correct device, but they cannot isolate guest RAM from a faulty or
malicious controller. This milestone therefore authorizes only QEMU's standard
emulated NVMe function and a temporary regular-file namespace.

## Deferred work

There are no media-changing commands, Delete I/O Queue commands, multiple
controllers, namespace enumeration, multiple active namespaces, multiple
outstanding requests, metadata, protection information, scatter/gather lists,
multi-page PRP lists, multipath, fabrics, reservations, namespace management,
hotplug, IOMMU support, partition parsing, filesystem or public application
ABI. Controller disable is the bounded queue-destruction mechanism in this
one-shot lifecycle.

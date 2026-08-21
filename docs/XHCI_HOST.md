# Bounded xHCI host-controller foundation

Sapote v0.4.0 owns one xHCI PCI function through the v0.3.0 PCI-resource,
dynamic-vector, MSI-X, and DMA APIs. It is a production-shaped controller
lifecycle, not a general USB stack: one controller, one interrupter, one direct
root-port device, one slot, and endpoint zero are the complete supported set.
Every hub, second device, non-control endpoint, stream, isochronous transfer,
class driver, and hotplug request is rejected rather than approximated.

The implementation is `src/kernel/xhci.c`; its public bounded handles are in
`include/sapote/xhci.h`. QEMU's emulated controller is the only installed proof
fixture. This milestone does not access host USB or PCI hardware.

## Sources fixed before implementation

The register and structure definitions come from Intel's current public
[USB and xHCI specification page](https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/universal-serial-bus-specifications.html),
not recalled layouts. That page now publishes both xHCI r1.2c and r2.0. This
bounded milestone deliberately implements the r1.2c interface and accepts only
controller revisions 1.0 through 1.2; r2.0 is a named unsupported-version
result rather than an approximation. The installed QEMU 11.1.0 fixture reports
xHCI 1.0. The relevant r1.2c clauses are:

- §4.2 for initialization order, §4.3 for root-port/device initialization, and
  §4.22.1 plus §7.1 for the BIOS/OS semaphore handshake;
- §5.3 for capability registers, §5.4 for operational and port registers,
  §5.5 for runtime/interrupter registers, and §5.6 for doorbells;
- §6.2 for input, slot, and endpoint contexts, §6.4 for TRBs and completion
  events, §6.5 for ERST alignment and size, and §6.6 for scratchpads; and
- §7 for checked extended-capability traversal and Supported Protocol port
  ranges.

USB request and descriptor validation follows the USB-IF
[USB 2.0 specification bundle](https://www.usb.org/document-library/usb-20-specification):
§9.3 defines the eight-byte setup packet, §9.4.3 defines `GET_DESCRIPTOR`, and
§9.6.1/Table 9-8 defines the 18-byte device descriptor. PCI discovery uses the
standard serial-bus/USB/xHCI class tuple recorded by the public
[PCI Code and ID Assignment specification](https://pcisig.com/specifications/industry_documents/PCI_Code-ID_r_1_19__v8_July_2025.pdf).
MSI-X capability parsing and programming remain centralized in the existing
v0.3.0 implementation. xHCI r1.2c §5.2.8 points to the PCI 3.0 MSI-X layout in
§6.8.2; the implementation was also checked against the current
[PCI Express Base specification publication](https://pcisig.com/specification-overview/pci-express-base),
whose current approved revision is 7.0.

The QEMU fixture follows the current
[QEMU USB emulation documentation](https://www.qemu.org/docs/master/system/devices/usb.html):
`qemu-xhci` plus one `usb-kbd` on `xhci.0` port 1. Local QEMU 11.1.0 help and
Ubuntu CI QEMU 8.2.2 device help were checked before the scenario was
installed. The fixture requests only their common `streams=off` property;
QEMU 8.2.2 exposes neither `msi` nor `msix` as a configurable property. The
guest still requires the controller's real PCI MSI-X capability and a normal
`msix_bind` result. The USB device uses the documented `bus`, `port`, and
`usb_version` properties.

The implemented register subset is intentionally visible here so offsets are
reviewable against xHCI r1.2c rather than inferred from C layout:

| Register family | Offsets used | Specification |
|---|---|---|
| capability | `CAPLENGTH/HCIVERSION` `00h/02h`, `HCSPARAMS1/2` `04h/08h`, `HCCPARAMS1` `10h`, `DBOFF` `14h`, `RTSOFF` `18h` | §5.3 |
| operational | `USBCMD` `00h`, `USBSTS` `04h`, `PAGESIZE` `08h`, `CRCR` `18h`, `DCBAAP` `30h`, `CONFIG` `38h` | §5.4.1–§5.4.7 |
| root port | `PORTSC` `400h + 10h × (port − 1)` from the operational base | §5.4.8 |
| primary interrupter | runtime `20h`, then `IMAN` `00h`, `IMOD` `04h`, `ERSTSZ` `08h`, `ERSTBA` `10h`, `ERDP` `18h` | §5.5.2–§5.5.2.3 |
| doorbell | controller doorbell `0`, device doorbell `4 × slot`, target endpoint ID `1` | §5.6 |

The command and event rings are 16-byte aligned (§6.4.1); CRCR, DCBAAP,
ERSTBA, and the sole event-ring segment are 64-byte aligned (§5.4.5,
§5.4.6, §5.5.2.3, and §6.5); input/output contexts use page-aligned storage
large enough for either 32- or 64-byte contexts (§6.2); scratchpads use the
selected 4 KiB page size (§6.6). Command and transfer completion meanings come
from §6.4.2.2 and §6.4.2.3. The implementation bounds CNR/reset and legacy
ownership by one second, command/transfer completion by two seconds, halt by
100 ms, and port reset by one second; every bound is enforced with the existing
monotonic clock. The one-second reset and legacy bounds cover the maxima stated
by §5.4.1 and §7.1, while the longer software completion bounds are explicit
Sapote policy rather than an unbounded wait.

## Typed ownership and states

The public types distinguish the controller claim, validated register regions,
command/event/control rings, ERST, DCBAA, scratchpads, input/output contexts,
selected port, slot, endpoint-zero transfer, interrupt binding, and stable proof
result. Controller transitions are:

```text
uninitialized -> discovered -> claimed -> prepared -> running
                                                |
                                                v
                                           stopping -> released
```

Only those edges are legal. Repeated, reversed, and skipped edges have distinct
status values. Partial initialization may move directly from discovered,
claimed, or prepared to released during rollback.

DMA allocations have CPU-owned, controller-owned, and reclaimed states. Ring
producer slots additionally carry CPU/controller ownership because a live xHCI
ring is a shared object: the CPU may publish only a CPU-owned TRB and may not
inspect a controller-owned event. The controller may start only after all eight
possible allocations are initialized and transferred:

1. one administration page for DCBAA, ERST, and the scratchpad-pointer array;
2. command ring;
3. event-ring segment;
4. input-context page;
5. output-context page;
6. endpoint-zero transfer ring;
7. descriptor receive page; and
8. one optional contiguous scratchpad allocation.

All are requested below 4 GiB. That satisfies a controller without `AC64` and
also makes the one proof layout independent of address-width capability. A
malformed scratchpad count or an allocation that violates the reported address
width is refused.

## Checked register map

Before changing controller state, checked addition and alignment prove that the
capability, operational, port-register, doorbell, runtime, selected interrupter,
and every traversed extended-capability range fit the claimed BAR. Independent
regions may not overlap. The selected interrupter is the documented nested
subregion of the runtime block. The extended-capability walker keeps a bounded
visited-offset set and refuses cycles, non-progress, misalignment, truncation,
and out-of-range next pointers.

MMIO accesses are volatile. Reserved fields are zero in generated contexts and
TRBs; read/modify/write operations preserve ordinary control bits, while W1C
status fields write only the observed change bits. A store fence precedes every
controller-visible publication and doorbell. Every readiness, ownership, halt,
reset, port, command, and transfer wait compares the existing monotonic clock
against a finite deadline.

## Initialization and interrupt order

The controller lifecycle is deliberately strict:

1. claim and map BAR 0 through the PCI-resource owner;
2. validate the complete reported register topology;
3. perform only the §7.1 BIOS/OS semaphore protocol if the legacy capability is
   present;
4. wait for controller-ready, halt, and reset;
5. validate version, slot/port/interrupter counts, 4 KiB page support, both
   allowed context sizes, scratchpads, and address width;
6. select exactly one connected USB 2 root port and allocate all bounded DMA;
7. install the handler and bind MSI-X entry zero while xHCI delivery is still
   disabled;
8. transfer every DMA object to controller ownership;
9. enable PCI bus mastering, then program DCBAAP, CRCR, ERSTSZ, ERSTBA, ERDP,
   and zero interrupt moderation (the ERSTBA write may immediately read DMA);
10. enable interrupter IE, then USBCMD INTE and RS.

The handler consumes only current-cycle event TRBs, updates ERDP with EHB,
clears IMAN IP and the observed USBSTS W1C sources, and returns the event ring to
controller ownership. Dynamic MSI-X dispatch then performs the normal local-APIC
EOI. No I/O APIC route or directed EOI participates in this path.

Command and transfer events must match type, cycle, exact TRB pointer, slot,
endpoint, completion code, and residual length. A port-change or unrelated event
is counted and ignored; it cannot complete a wait.

## Teardown and isolation boundary

If a slot was enabled it is disabled first. Interrupt sources are masked, RS and
INTE are cleared, and the halt bit must arrive before PCI bus mastering is
disabled. The MSI-X binding and handler are then removed before device-owned
objects are reclaimed. DMA is released in reverse order, BAR mappings are
removed, and the claim is released. The final PCI, MMIO-arena, DMA, frame,
vector, handler, MSI-X, and bus-master resource snapshot must equal the entry
snapshot.

Sapote still has no IOMMU. Bounds, typed ownership, and bus-master ordering make
driver mistakes observable and prevent known use-after-free paths; they do not
isolate RAM from a malicious or defective device. v0.4.0 deliberately does not
add an IOMMU.

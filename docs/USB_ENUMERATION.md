# Minimal USB device-descriptor proof

The `xhci` scenario is one end-to-end enumeration of QEMU's emulated
`usb-kbd`. It proves the reusable xHCI controller lifecycle without installing a
keyboard service or general USB subsystem. The PS/2 keyboard and all inherited
public interfaces are unchanged.

## Fixture and stable contract

The Makefile adds exactly this guest-only fixture to scenario 34:

```text
-device qemu-xhci,id=xhci,streams=off
-device usb-kbd,bus=xhci.0,port=1,usb_version=2
```

The guest exit value is `0x31`; QEMU reports host status 99. The fixture uses no
`usb-host`, VFIO, host device file, or physical hardware. Normal boots without
this PCI function record the Boot Ledger's explicit neutral `xHCI fixture
absent` outcome.

Ubuntu QEMU 8.2.2 exposes no `msi` or `msix` command-line property for
`qemu-xhci`; newer local QEMU does. The portable fixture therefore does not
pretend either property exists. MSI-X remains mandatory inside the guest:
Sapote must discover the PCI capability, bind its normal dynamic vector, and
observe the exact interrupt-count transition or the scenario fails.

The stable success evidence is:

```text
Sapote: xHCI controller ready
Sapote: USB device descriptor DMA completed: 18 bytes
Sapote: xHCI MSI-X descriptor completion count 1
Sapote: xHCI DMA ownership CPU-CONTROLLER-CPU complete
Sapote: xHCI teardown complete
ST XHCI descriptor 18 msix 1 ownership CPU-CONTROLLER-CPU teardown clean robustness 19
```

It intentionally omits physical and virtual addresses, timing, PCI topology,
serial number, and vendor/product identifiers.

## Enumeration sequence

After the host foundation reaches running state, the proof:

1. resets the one connected USB 2 root port and requires CCS, PED, and PRC;
2. submits Enable Slot and accepts only slot 1;
3. fills a context-size-selected input context with the root-port number, speed,
   context-entry count, endpoint-zero type, maximum packet size, dequeue pointer,
   DCS, and average TRB length;
4. submits Address Device only after port, slot, input context, output context,
   and DCBAA state are valid;
5. fills the 4 KiB receive page with `0xA5`, transfers it to controller
   ownership, and publishes Setup/Data/Status TRBs for the USB 2.0 §9.4.3
   request `80 06 00 01 00 00 12 00`;
6. snapshots the bound interrupt counter, rings slot 1's endpoint-zero doorbell,
   and waits on a monotonic deadline;
7. requires the transfer event to point to the exact Status TRB, name slot 1 and
   endpoint ID 1, report success or a valid short packet, and leave a residual
   no larger than 18;
8. requires the programmed MSI-X counter to advance by exactly one;
9. returns the receive page to CPU ownership and validates an exact 18-byte
   device descriptor; and
10. disables the slot and performs the complete host teardown.

Descriptor validation checks `bLength`, `bDescriptorType`, BCD syntax,
`bMaxPacketSize0`, a nonzero configuration count, and the device-class zero
subclass/protocol relationship. It does not depend on QEMU's identifiers. At
least one of the first 18 sentinel bytes must change, and every byte after the
requested range must remain `0xA5`. The CPU reads those bytes only after the
matching transfer event returned ownership.

## Controlled robustness matrix

The first 17 controls run in the required xHCI-foundation stage against local
synthetic objects and explicit validation helpers. The last two run inside the
descriptor's own Boot Ledger stage. No control sends malformed input to QEMU or
another system.

| # | Deterministic control | Required refusal/evidence |
|---:|---|---|
| 1 | outside/overflowing BAR span | named range or overflow status |
| 2 | cyclic, non-progressing, or out-of-range xECP | three distinct statuses |
| 3 | legacy semaphore never clears | finite legacy timeout |
| 4 | halt or reset never completes | independent finite timeouts |
| 5 | bad page/context/address-width combination | named unsupported result |
| 6 | scratchpad count/size overflow | scratchpad overflow |
| 7 | displaced/overflowing ring, ERST, or context | alignment/layout refusal |
| 8 | wrong cycle or producer ownership | ring cycle/ownership refusal |
| 9 | mismatched event field | event ignored; wait not completed |
| 10 | incomplete handler/MSI-X/event-ring readiness | interrupt enable refused |
| 11 | CPU-owned referenced DMA | doorbell refused |
| 12 | unprepared DMA | bus mastering refused by PCI owner |
| 13 | incomplete port/slot/context state | Address Device refused |
| 14 | short, oversized, or inconsistent descriptor | three validation results |
| 15 | wrong DMA ownership | access/release refused |
| 16 | every partial initialization boundary | resource count returns to entry |
| 17 | controlled teardown observation hook | freed state is unobservable |
| 18 | temporary descriptor loses one prerequisite | dependency validation fails |
| 19 | temporary alternate xHCI exit `0x32` | host/guest exit contract rejects it |

The installed proof receipt records descriptor length 18 and MSI-X delta 1.
Completion code, returned length, vector delta, ownership round trip, sentinel,
slot disablement, teardown, and entry/final resource snapshots must all agree
before scenario success is possible.

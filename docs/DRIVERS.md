<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded device drivers

Sapote drives three controllers completely — xHCI, NVMe and virtio-net — and
each of those owns a file, because each of them moves data. This page is about
the other thirteen: real devices from Intel, Realtek, AMD and Cirrus Logic, and
one display interface three emulators implement, that Sapote binds, brings to a
defined state, identifies against their own specifications, and releases.

Binding is the part of a driver that has to be right before anything else can
be. A driver that cannot tell whether the device in front of it is the device
it was written for has no business programming a queue. Thirteen devices bound
correctly is a broader statement about the machine boundary than one device
driven completely.

## What every driver does and does not do

Every driver here:

- matches on vendor, device, class and subclass together, never on one of them;
- reaches its device either through configuration space only, or through
  exactly one memory BAR claimed and mapped uncached by the typed PCI
  substrate;
- performs the reset its specification defines, where one exists, and waits for
  the device's own completion signal under a one-second monotonic deadline;
- reads the registers that identify the device and checks them against a
  property the specification guarantees, not against a value that happened to
  be observed;
- unmaps and releases everything, and the matrix compares the frame, paging,
  DMA, PCI, vector and MSI-X census from before the first bind to after the
  last one.

No driver here enables bus mastering, and `make verify` refuses a build in
which one tries. Sapote has no IOMMU, so a bus-mastering device is treated as
able to reach all of physical memory; a driver that only touches registers is
one that *cannot* corrupt the kernel rather than one that is merely not
expected to. No driver here writes configuration space either — the four
chipset drivers read a machine that is actively decoding legacy I/O for the
console the tests read.

## The thirteen drivers

| # | Device | ID | Access | Reset | Identity checked against |
| --- | --- | --- | --- | --- | --- |
| 1 | Intel 82441FX host bridge | `8086:1237` | configuration | none | PAM0's reserved attribute field reads zero |
| 2 | Intel 82371SB PIIX3 ISA bridge | `8086:7000` | configuration | none | every routed PCI interrupt names a legal ISA interrupt |
| 3 | Intel 82371SB PIIX3 IDE | `8086:7010` | configuration | none | a decodable programming interface, per-channel decode enables |
| 4 | Intel 82371AB PIIX4 power management | `8086:7113` | configuration | none | an enabled power-management base is a real I/O base |
| 5 | Intel 82540EM Gigabit Ethernet | `8086:100E` | BAR0 memory | `CTRL.RST` self-clears | EEPROM checksum 0xBABA, EEPROM and receive-address MAC agree, MAC is unicast |
| 6 | Intel 82574L Gigabit Ethernet | `8086:10D3` | BAR0 memory | `CTRL.RST` self-clears | as above, through the 82574's different EEPROM read encoding |
| 7 | Intel 82801IR ICH9 SATA AHCI | `8086:2922` | BAR5 memory | `GHC.HR` self-clears | major version 1, at least one implemented port, no more ports than `CAP.NP` allows |
| 8 | Intel 82801I ICH9 HD Audio | `8086:293E` | BAR0 memory | `GCTL.CRST` low then high | major version 1, a non-zero capability register |
| 9 | Realtek RTL8139 Fast Ethernet | `10EC:8139` | BAR1 memory | `CR.RST` self-clears | a hardware version identifier in the transmit configuration register, MAC neither zero nor broadcast |
| 10 | Intel 82801DB USB 2.0 EHCI | `8086:24CD` | BAR0 memory | `USBCMD.HCRESET` self-clears | interface version 1.0, a capability block long enough to hold itself, operational registers inside the window, at least one root port, and a halted controller after reset |
| 11 | Cirrus Logic GD5446 display | `1013:00B8` | BAR1 memory | none | the extension lock reads back the key when open and 0x0F when closed, and the CRTC chip identifier names a GD5446 |
| 12 | Bochs Display Interface | `1234:1111` (subclass 0x80) | BAR2 memory | none | a documented interface version, and a memory-size register that agrees to the byte with the prefetchable framebuffer BAR |
| 13 | AMD Am79C970A PCnet-PCI II | `1022:2000` | BAR1 memory | `S_RESET` read | JEDEC manufacturer 0x001 (AMD), fixed bit set, the chip identity reads the same through the 16-bit and 32-bit register files |

Nine are Intel parts, one Realtek, one AMD, one Cirrus Logic, and one is the
Bochs Display Interface, which is not a chip but a display programming
interface Bochs, QEMU and VirtualBox all implement and Linux drives through
`bochs-drm`. Seven perform a real reset; the rest have no reset to perform, and
the matrix records that as a declared property rather than inferring it.

The Bochs entry is matched on subclass 0x80 rather than 0x00 on purpose. A
machine can carry both a standard VGA controller and this interface under the
same vendor and device identifier, and the one the firmware set the boot
display mode on is not somewhere a bind-time driver should be writing.

Three of these are worth reading in full. The Intel Gigabit driver does not trust
one source for the station address: it reads the address out of the receive
address registers, reads it again word by word out of the EEPROM, sums all
sixty-four EEPROM words and requires the total to be the checksum Intel
specifies. Any one of those could be a coincidence; three agreeing is the part
telling the truth about itself. The PCnet driver resets the device by *reading*
its reset register — the datasheet gives that register no data — then reads the
chip identity through the narrow register file, changes the part to 32-bit mode
with the single documented write, and reads the identity again at the wider
stride. The two reads must agree. The Cirrus driver has to decide which of the
two CRTC address pairs is live before it can reach the chip identifier, the
same decision a VGA driver has made since 1987, and it puts both the
addressing and the extension lock back exactly as it found them.

## Where it runs

`src/kernel/driver.c` holds all thirteen drivers and the matrix that binds
them.
`include/sapote/driver.h` is the contract. Two typed Boot Ledger stages own
them:

- **bounded PCI driver matrix foundation** validates the declared matrix
  itself — twelve controls covering duplicate identities, absent vendor
  identifiers, access-mode consistency, class codes, accessor bounds, the reset
  deadline, and the configuration-space decode helpers.
- **installed PCI driver matrix probe** binds every declared device that is
  present. It is a neutral-skip stage: a machine carrying none of the thirteen
  is a machine this stage has nothing to do on.

The probe stage runs where its scenarios attach the hardware, the same way the
xHCI, NVMe, filesystem and process proofs do. Binding resets seven devices, and
a proof that resets hardware belongs in the scenario that asked for the
hardware.

## Evidence

| Scenario | Machine | What it proves |
| --- | --- | --- |
| `driver-matrix` | i440fx with all thirteen devices attached and four pinned station addresses | all thirteen present, all thirteen bound, seven resets observed, every pinned address read back, census equal |
| `driver-matrix-builtin` | i440fx as it comes | the five built-in devices bind and the eight absent ones are reported absent with no identity and no driver bound |

Both scenarios read the Boot Ledger receipt as well as the matrix result, so a
driver that bound without the ledger recording it, or a ledger entry without a
matching bind, fails.

### Proving a driver read a real device

Four of the thirteen carry a station address, and the `driver-matrix` scenario
gives each of those four an address chosen on the host's QEMU command line
rather than one QEMU would have picked. The kernel then requires each driver to
report exactly that address.

That value travels from the Makefile, into QEMU's device model, into the part's
own EEPROM or address registers, out through whichever register sequence that
particular part demands — a checksummed EEPROM read for the Intel Gigabit
parts, six byte reads for the Realtek, the address PROM for the AMD — and back
into a comparison. A driver that returned a plausible-looking address it had
not actually fetched would have to invent all four of these exactly, and
`make verify` checks that the addresses in the Makefile and the addresses in
the kernel's test are the same four.

## Limits

These are binding and identification drivers. None of them transmits a frame,
reads a sector, or plays a sample; none programs a descriptor ring, allocates
DMA, or takes an interrupt. `docs/ARCHITECTURE.md` lists which devices Sapote
drives completely — this page is deliberately not that list. Adding data
movement to any of these needs DMA ownership, an interrupt vector, and its own
evidence, which is a separate increment per device rather than an extension of
this one.

The matrix is also bounded to devices Sapote's test machine can present. It has
no hotplug, no rebinding, no driver ordering beyond the declared one, and no
concept of a device staying bound after the probe returns.

Several drivers leave their device in the state their reset defines rather than
the state they found it in: the AHCI controller is left enabled, the HD Audio
controller out of reset, the EHCI controller halted, and the PCnet part in
32-bit register mode. That is the state each specification says a freshly bound
controller is in, and it is what the next thing to drive them would want. The
two display drivers, by contrast, put back everything they touched, because
something else may be looking at a display.

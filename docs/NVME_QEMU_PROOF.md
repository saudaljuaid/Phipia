# QEMU NVMe fixture and proof

Scenario 35 is the only boot that installs an NVMe namespace. The Makefile
creates a new temporary regular file under `build/tests/nvme`, attaches it only
to QEMU's standard emulated NVMe controller and presents the namespace read-only
to the guest. It never names a host block device and never uses passthrough.

## Fixture

`tools/make-nvme-fixture.py` creates exactly sixteen 4096-byte logical blocks.
All bytes start as zero except logical block address 8. Byte `i` in that block
is `(i * 37 + 11) & 0xff`. The guest knows only the LBA, block size and byte
formula; stable output does not expose the temporary host path.

The checked QEMU 11.1.0 command surface accepts this bounded device shape:

```text
-blockdev driver=file,filename=<temporary-regular-file>,node-name=nvme-file,read-only=on,auto-read-only=off
-blockdev driver=raw,file=nvme-file,node-name=nvme-raw,read-only=on
-device nvme,serial=sapote-fixture,drive=nvme-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1,mqes=1
```

The `file` and `raw` nodes are both read-only. QEMU's supported NVMe controller
and namespace options are documented at
<https://www.qemu.org/docs/master/system/devices/nvme.html>. `make verify` and
the evidence workflow also record `qemu-system-x86_64 -device nvme,help` so a
runner-side syntax change fails visibly.

## End-to-end acceptance

The normal Boot Ledger stage first runs the twenty controller-local synthetic
controls. With the fixture present, the following optional stage discovers the
controller by PCI class, claims and maps it, prepares its queues and buffers,
binds masked MSI-X, enables it in specification order, identifies the controller
and namespace, creates the I/O queues, submits Read at LBA 8 and waits on a
bounded monotonic deadline. The test accepts only one matching CQ completion
and an exact `+1` MSI-X count between Read submission and completion. It never
calls the handler and never injects a software interrupt.

After content validation and teardown, the only scenario-specific stable line
is:

```text
ST NVME read 4096 msix 1 ownership CPU-CONTROLLER-CPU teardown clean robustness 22
```

The surrounding stable facts report controller readiness, namespace readiness,
4096 block bytes, MSI-X count 1, completed ownership and clean teardown. They do
not contain addresses, topology, timing, paths or controller serial values.
Guest exit `32h` maps through QEMU's debug-exit device to host exit 101. A
controlled alternate guest value `33h` must not satisfy that contract.

## Controlled robustness matrix

Controls 1–20 execute with synthetic values or explicit guest-local hooks in
the NVMe foundation stage. Controls 21–22 exercise the Boot Ledger and scenario
contract. No control sends malformed commands to the emulated namespace.

| # | Control | Required result |
| ---: | --- | --- |
| 1 | register/doorbell outside BAR, overflowing or misaligned | named rejection before state change |
| 2 | maximum CAP queue field and overflowing doorbell geometry | 65536 decoded safely; impossible geometry rejected |
| 3 | unsupported command set or 4 KiB page combination | distinct named rejection |
| 4 | enable/disable deadlines | finite, distinct timeout results |
| 5 | malformed Admin queue | misalignment and overflow rejected |
| 6 | malformed I/O queue | short allocation and multiplication overflow rejected |
| 7 | wrong completion phase or CPU access during device ownership | refused |
| 8 | duplicate, zero or reserved CID | distinct duplicate/range result |
| 9 | malformed Identify Controller/Namespace data | refused |
| 10 | absent or inactive namespace | distinct absent/inactive result |
| 11 | unsupported LBA format, metadata or protection information | distinct result |
| 12 | zero, out-of-range or overflowing block range | distinct result |
| 13 | PRP outside allocation or crossing a page | refused |
| 14 | phase, CID, SQID or status mismatch | never satisfies the wait; reported |
| 15 | interrupt delivery before handler, MSI-X and queue readiness | refused |
| 16 | doorbell before referenced DMA belongs to controller | refused |
| 17 | bus mastering before all seven allocations are ready | refused by PCI resource layer |
| 18 | CPU inspection or release of controller-owned memory | refused by ownership API |
| 19 | cleanup after every DMA/queue allocation boundary | entry/exit resource snapshots equal |
| 20 | controlled teardown race | freed state detected but unobservable |
| 21 | omit one required NVMe proof capability | Boot Ledger dependency validation fails |
| 22 | alternate scenario exit value | host contract rejects it |

Every live path, including partial MSI-X and queue acquisition, runs the same
reverse teardown. Closing validation requires no retained PCI claim or mapping,
DMA allocation or owner, physical frame, vector, handler, MSI-X binding or bus
master.

## Evidence workflow

`.github/workflows/nvme-milestone.yml` runs `make verify`, all 35 scenarios in
each of ten complete serial TCG sweeps, and one complete sweep for each hardware
accelerator present on its runner. It writes an explicit availability record
when KVM is unavailable. Its artifact retains the verified ISO, checksums,
proof transcript, all ten sweep logs, accelerator record, robustness result and
all scenario ISOs needed for a separately recorded WHPX sweep on an available
Windows host.

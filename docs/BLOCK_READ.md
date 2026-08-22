# Minimal block-read contract

The v0.5.0 storage contract is deliberately one operation: receive one logical
block from namespace 1 into a caller-designated DMA page through PRP1. The only
installed consumer is the Boot Ledger's QEMU proof. No public userspace or
filesystem ABI exists.

## Validated inputs

`struct nvme_namespace_selection` records an active namespace identifier,
capacity in logical blocks, the selected LBA-format index and logical-block
byte count. `struct nvme_logical_block_range` carries a first LBA and a count.
`struct nvme_prp_read_buffer` carries one bounded DMA allocation, its one-page
payload offset and length, ownership and whether the payload changed during the
controller-owned interval.

Identify Controller must report:

- 64-byte submission entries and 16-byte completion entries within the
  controller's supported size ranges;
- the same NVMe version as `VS`;
- one I/O controller;
- a namespace limit that admits identifier 1; and
- an `MDTS` value that either declares no limit or admits 4096 bytes.

Because Identify Controller `NN` is an implementation namespace limit rather
than an attached-namespace census on supported QEMU versions, Sapote next uses
Identify Active Namespace ID List (`CNS=02h`) in the existing namespace buffer.
The list must be exactly identifier 1 followed only by zero entries. This is
the guest-side proof that no second namespace is active.

Identify Namespace 1 must then be nonzero and active. `NSZE`, `NCAP` and `NUSE` must
be ordered and nonzero as applicable; the selected `FLBAS` index must lie
within `NLBAF`; metadata must be zero; metadata may not be transferred as a
separate buffer; `DPS` may not select protection information; and the selected
LBA data size must be exactly 4096 bytes. The proof rejects absent, inactive or
additional namespaces rather than silently choosing one.

The request count must be one. Zero length, `first + count` overflow and a
range beyond `NSZE` have distinct results. The data address must be page
aligned, contained in its DMA allocation, no longer than one Sapote page and
must not cross a page boundary. PRP2 and PRP lists remain zero and unsupported.

These layouts and rules come from NVMe Base 2.4 §§4.1.1, 4.2.1–4.2.4, 4.3.1
and 5.2.14, NVM Command Set 1.3 §3.3.4 and §4.1.5.1, and NVMe over PCIe
Transport 1.4 §§3.1.1–3.2. The official documents are linked from
`docs/NVME_CONTROLLER.md`.

## Command contract

Create I/O Completion Queue creates CQ 1 at depth 2, physically contiguous,
interrupt-enabled and bound to MSI-X vector index zero. Create I/O Submission
Queue then creates SQ 1 at depth 2, physically contiguous and associated with
CQ 1. Each Admin command receives a nonzero, non-reserved identifier and only
one identifier may be outstanding.

NVM Read uses opcode `02h`, namespace identifier 1, `SLBA` in command dwords
10–11, zero-based `NLB == 0` in dword 12 for exactly one logical block, PRP1 for
the payload, and no metadata pointer. It contains no FUA, limited-retry,
protection, directive, SGL or multi-page option. The proof never constructs an
NVM Write or any other media-changing opcode.

The designated 4 KiB payload sits between one 4 KiB sentinel page on either
side inside a three-page DMA allocation. All 12 KiB initially contain `A5h`.
The full allocation transfers to the controller before Read is submitted. The
MSI-X handler returns it to the CPU only for the exact successful completion.
The CPU then requires every payload byte to match the deterministic fixture and
every surrounding byte to remain `A5h`. A payload differing from the sentinel
proves a device-owned change interval; the ownership API and handler path prove
that the CPU did not modify or inspect it during that interval.

## Result

`struct nvme_read_proof` is copied into stable installed state only after a
successful teardown and a zero-leak resource comparison. It records only:

- controller and namespace readiness;
- logical-block bytes;
- the MSI-X interrupt-count delta around the Read command;
- successful content and sentinel checks;
- the CPU–controller–CPU ownership cycle;
- ignored completion count;
- controlled robustness count; and
- teardown completion.

It intentionally records no address, PCI topology, host path, timing, serial
number or other environment-specific identity. Ordinary boots without an NVMe
fixture produce a neutral `NVMe fixture absent` Boot Ledger receipt.

## Deferred work

The next storage milestone may place a bounded read-only filesystem on this
contract. It must not widen this milestone retroactively. Partition discovery,
read aggregation, caching, asynchronous public I/O, write paths and all other
controllers require separately designed contracts and tests.

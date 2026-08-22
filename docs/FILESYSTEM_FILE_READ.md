# Minimal root-file read contract

The only v0.6.0 file operation is a private installed proof that reads
`SAPOTE.BIN` from one validated FAT16 root. C controls the controller and
lifecycle; Rust alone interprets filesystem bytes. No symbol provides a public
block or file API.

## Typed lifecycle

The filesystem candidate carries only the identified namespace block count and
4096-byte LBA size. Rust turns its BPB into pointer-free checked geometry. The
root query, root entry, FAT state and one-cluster extent are distinct values,
so an unchecked field cannot be substituted for a checked LBA.

The C path advances through `unopened`, `session-ready`,
`block-controller-owned`, `block-CPU-owned`, `volume-validated`,
`file-located`, `file-read`, `stopping` and `released`. Repeated, reversed,
skipped and cross-generation transitions return named failures. The private
NVMe session permits one live instance and ordinals 1–4 only.

The controller is initialized once. It identifies active namespace 1 and
validates 4096 logical blocks of 4096 bytes before opening the session. The
four one-block reads are:

1. BPB at volume sector zero;
2. the metadata-derived first FAT sector;
3. the metadata-derived fixed-root sector; and
4. the metadata-derived cluster-2 data sector.

Each submission uses the existing NVM Read opcode `02h`, namespace ID 1,
zero-based NLB value zero and a single page-aligned PRP1. This inherits NVMe
NVM Command Set 1.3 §3.3.4 and §4.1.5.1, NVMe Base 2.4 §§4.1.1,
4.2.1–4.2.4, 4.3.1 and 5.2.14, and NVMe over PCIe Transport 1.4. A completion
must match its ordinal CID, I/O SQID, phase and success status through the
programmed MSI-X vector; exactly one interrupt-count transition is accepted.

## DMA ownership

One reusable three-page contiguous allocation contains a one-page payload with
one sentinel guard page on either side. Before every read C obtains CPU
ownership, fills all three pages with `A5h`, publishes the bytes with the
existing compiler/store ordering, transfers the allocation to the controller,
submits and rings the I/O doorbell. Neither C nor Rust can view, parse, copy,
reuse or release the payload in controller ownership.

After the matching MSI-X completion, the completion path returns the allocation
to CPU ownership. C verifies both complete guard pages and proves that the
payload changed during that read before Rust sees a slice. The session carries
a separate current-read change bit so a prior successful read cannot satisfy a
later read; its cumulative bit is used only for the final aggregate evidence.
Rust parses BPB, FAT and root blocks immediately. For the data block Rust
validates every deterministic byte and SHA-256; only then does C copy exactly
128 bytes into the bounded CPU-owned result.

The deterministic payload is `byte[i] = (i * 73 + 19) mod 256`. Its SHA-256 is
`D399F065C9F21E2FD51E2AEADB7768EAB7E6E45E5150F31227C9711934A4D1D3`.
The stable proof is installed only after the file buffer is CPU-owned, four
reads and four MSI-X deltas are proven, and teardown is complete.

## Teardown and security boundary

Reverse teardown disables `CC.EN` and observes `CSTS.RDY=0` within the inherited
deadline before DMA reclamation. It then masks MSI-X, disables PCI bus
mastering, returns every DMA object to CPU ownership, releases queues and DMA,
unbinds/frees the vector, unmaps BAR0 and releases the claim. Entry and exit
PCI, DMA, vector, MSI-X and frame censuses must match, and the closing Boot
Ledger proof additionally requires the filesystem session to be released.

Sapote still has no IOMMU. These bounds and ownership states prevent correct
software from misusing memory; they do not isolate guest RAM from a faulty or
malicious bus-mastering device.

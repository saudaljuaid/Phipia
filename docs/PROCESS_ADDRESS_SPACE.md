<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Private process address space and W^X

The v0.7.0 address-space foundation owns exactly one private four-level
hierarchy. It is a proof object, not a public mapping or process API. Its only
user mappings are one RX image page and four RW/NX stack pages; the page below
the stack is genuinely absent and the null page remains absent.

AMD64 Architecture Programmer's Manual Volume 2, publication 24593 revision
3.44 (6 March 2026), Sections 4.8, 5.3.1, 5.4 and 5.6 define four-level walking,
48-bit canonical addresses and the conjunction of `U/S`, `R/W` and XD across
ancestors and the leaf. Intel SDM version 092, Volume 3A Chapter 4 is the
independent cross-check. Sapote treats a mapping as user-accessible only when
every traversed present entry has `U/S=1`; a writable or executable denial at
any level wins.

## Fixed layout

| Range | Pages | Effective permission |
| --- | ---: | --- |
| `0x0000400000000000` | 1 | user read/execute |
| `0x0000400000200000` | 1 | absent guard |
| `0x0000400000201000..0x0000400000204fff` | 4 | user read/write, NX |

Both reservations are page-aligned lower-half canonical addresses in a PML4
branch unused by the installed kernel. The image, guard and stack are mutually
disjoint. The builder refuses any other virtual address, size, permission or
mapping kind.

## Supervisor mapping intent

The private hierarchy is reconstructed; live page-table entries are not copied.
Paging records successful runtime supervisor mappings as semantic tuples of
virtual address, physical address, length and permission. Private construction
builds the installed identity/device policy, validates it, and replays these
tuples. Heap, thread stacks, PCI MMIO, DMA buffers, interrupt state, Boot Ledger
storage, page-table frames and kernel image pages therefore remain reachable
with their original memory types and read/write/execute intent, but every
ancestor and leaf remains supervisor-only.

The registry is fixed storage and can refuse `supervisor intent full`; it is
not a public mapping database. Exact successful unmap and protect operations
remove or update their entry. A software audit before CPL3 entry requires zero
kernel user bits, exactly five user leaves and zero W+X leaves.

## Executable-alias narrowing

Physical frames are identity mapped RW/NX while initialized. Before the image
gets a user executable leaf, paging narrows that exact identity alias to
supervisor RO/NX in both the live and private hierarchies. If the identity leaf
was a 2 MiB page, the operation allocates one page table, expands the semantic
mapping into 512 4 KiB leaves, changes only the image leaf and reloads the
active CR3. This prevents a stale huge writable alias from defeating W^X.

The pre-entry walk proves:

- the user image resolves to the CPU-owned frame as RX and is not writable;
- the same physical address through the supervisor identity alias is RO/NX;
- every stack leaf resolves to its expected frame as RW/NX;
- guard and null translations report not mapped;
- the kernel PML4 branch and all its descendants contain no user bit; and
- the hierarchy-wide audit contains exactly five user leaves and no W+X leaf.

## Ownership and teardown

Address spaces and narrowed aliases carry private generation tokens. Invalid,
stale, repeated, cross-object and wrong-state operations return named paging
statuses. States are invalid, building, installed, active and released.

CR3 activation and restoration require interrupts disabled and exact current
root identity. The return interrupt restores the installed kernel CR3 before
any unmap or release. Teardown then removes stack mappings in reverse order,
removes the image mapping, restores the live identity alias (and collapses any
temporary split), recursively frees every private table, and finally frees
stack and image frames. `paging_verify` and a pre/post census require the
installed root and table count to match afterward.

There is no PCID, SMP, TLB shootdown, demand paging, COW, ASLR, huge user page,
user mapping API or concurrent process. Those require separate contracts.

## Separate v0.8.0 BusyBox owner

The Linux ABI proof is a second typed consumer of this foundation; it does not
alter the v0.7.0 image, stack, vector `0x81`, transcript, or exit. Its exact
high lower-half layout is declared in `paging.h`: nine initial image pages,
four stack pages above an absent guard, four bounded heap pages, and one fixed
anonymous page. The Rust BusyBox parser supplies four measured load segments,
and C installs each page with its final R, RX, R, or RW/NX conjunction. Six
identity aliases covering executable file bytes are narrowed RO/NX before the
private hierarchy becomes active.

The process states are candidate, building, installed, running, exiting,
stopping, and released. Only the measured `brk`/`mmap` sequence can add the
preallocated heap and anonymous leaves. The real LSTAR return restores kernel
CR3 before reverse-order unmapping and table/frame release. A census includes
frames, table counts, DMA, PCI claims, vectors, MSI-X, filesystem session,
syscall state, boundary state, interrupt state, and CR3; the BusyBox result is
published only when it exactly matches the pre-proof census.

# Device-foundation verification record

This record covers the BAR, dynamic-vector, MSI-X, DMA, and VirtIO RNG proof
increment. The measured source baseline was
`55dd4beabafdbe9f6efa4e521ff29815095db1e8`. Tests used QEMU 11.1.0, Clang
22.1.8, Rust 1.97.0, and the existing cross-binutils link contract.

## Executed negative controls

The controls below execute during boot or as a source/host contract assertion;
they are not uncalled helper branches. A passing `device-substrate` boot reports
all fourteen in its stable scenario line.

| # | Control | Observed refusal or proof |
| ---: | --- | --- |
| 1 | Probe a BAR while decode is enabled. | `PCI_RESOURCE_STATUS_DECODE_ENABLED`. |
| 2 | Inject failure after a BAR write. | `PCI_RESOURCE_STATUS_INJECTED_FAILURE`; command and every BAR read back exactly. |
| 3 | Present a 64-bit BAR without its upper pair. | `PCI_RESOURCE_STATUS_MALFORMED_64_BIT_PAIR`. |
| 4 | Validate overflowing, overlapping, and allocator-RAM-aliasing MMIO ranges. | Named range-overflow, MMIO-overlap, and `PCI_RESOURCE_STATUS_MMIO_RAM_OVERLAP` refusals. |
| 5 | Request fixed controller, self-test, IST, and spurious vectors. | Each returns `INTERRUPT_VECTOR_STATUS_RESERVED`. |
| 6 | Fill the dynamic interval, allocate once more, then release one handle twice. | `INTERRUPT_VECTOR_STATUS_EXHAUSTED` and `INTERRUPT_VECTOR_STATUS_DOUBLE_RELEASE`; allocation count returns to zero. |
| 7 | Place an MSI-X table past the sized BAR. | `MSIX_STATUS_TABLE_OUTSIDE_BAR`. |
| 8 | Enable delivery without an installed handler. | `MSIX_STATUS_HANDLER_NOT_INSTALLED`. |
| 9 | Inject failure after the live VirtIO handler is installed. | Complete reverse rollback; no binding, handler, vector, or mapping remains before the real bind. |
| 10 | Request non-power-of-two and impossible DMA alignment. | Bounded frame allocation is refused without changing allocation counts. |
| 11 | Transfer before initialization, forge a copied handle, transfer twice, reclaim twice, and release while device-owned. | Named not-prepared, wrong-owner, bad-handle, and double-free results; allocation counts return to their baseline. |
| 12 | Enable bus mastering before both VirtIO allocations are initialized and device-owned. | `PCI_RESOURCE_STATUS_DMA_NOT_PREPARED`; the command register remains non-mastering. |
| 13 | Remove one proof prerequisite both by shortening the list and by replacing a member at unchanged cardinality. | Both local descriptor controls are rejected before the fixture is touched. |
| 14 | Mutate the new guest exit from `0x30` to `0x31`. | The exit self-test rejects the mutation; the host contract remains 97. |

The closing installed proof requires zero claims, MMIO mappings and arena pages,
bus masters, contiguous frame records, DMA allocations, vectors, handlers, and
MSI-X bindings.

## End-to-end evidence

The standard `virtio-rng-pci` fixture was enumerated normally as `1AF4:1044`.
The proof claimed its modern capabilities, allocated a bounded split virtqueue
and receive page below 4 GiB, installed one MSI-X binding, transferred both DMA
allocations to the device, and submitted one 64-byte request. The installed
checks observed one real interrupt, used index `0 -> 1`, descriptor id zero,
used length 64, nonzero device-written bytes, CPU -> device -> CPU ownership,
and complete teardown. The source assertion also permits exactly one occurrence
of the proof handler call syntax: the handler definition itself, so the proof
cannot directly inject delivery.

## Complete-suite matrix

| Executor | Complete sweeps | Scenario boots | Result |
| --- | ---: | ---: | --- |
| TCG | 10 | 330 | 330 passed |
| WHPX | 1 | 33 | 33 passed |

The complete matrix was repeated after delayed review fixes strengthened BAR
decode read-back, private DMA authority, Boot Ledger outcome validation, and
wrong-vector reporting. The repeated matrix was again 330/330 under TCG and
33/33 under WHPX with no additional flake.

`make verify` also passed from a clean build with warnings promoted to errors.
Its binary checks found no undefined symbols, unresolved relocations, RWX load
segment, or floating-point, MMX, SSE, or AVX instructions. All inherited stable
transcripts and exit values passed the host comparator.

One preliminary TCG attempt, before the ten recorded sweeps, failed in the
inherited clock-agreement proof: PM-timer elapsed time was 279,441,381 ns while
the local-APIC interval was 200,000,000 ns. It occurred before PCI-resource,
vector, DMA, or fixture execution. The affected complete sweep was discarded
and rerun serially; the rerun passed all 33 scenarios. No other flake occurred.

## v0.5.0 NVMe evidence contract

The storage increment retains the inherited device-substrate evidence above
and adds scenario 35. `docs/NVME_QEMU_PROOF.md` is the complete twenty-two-item
control matrix. The live proof uses only QEMU's standard emulated NVMe PCI
function and a freshly created regular-file namespace that is read-only to the
guest. It must observe 4096 deterministic bytes written into the middle of a
guarded guest DMA allocation, one matching completion through MSI-X entry zero,
unchanged sentinel pages, CPU → controller → CPU ownership and an entry/exit
resource snapshot with no retained claim, mapping, frame, DMA object, vector,
handler, binding or bus master.

`.github/workflows/nvme-milestone.yml` retains `make verify`, every individual
scenario inside ten complete serial TCG sweeps and an explicit accelerator
record. It does not probe or use KVM because doing so requires the excluded host
device file `/dev/kvm`. It packages the ISO, SHA-256 checksums, proof transcript,
ten TCG result sets, accelerator record and robustness result. A locally
available WHPX accelerator consumes the retained per-scenario ISOs; its final
sweep record is added to the release evidence. KVM remains an explicitly
unmeasured evidence gap under this task's authorization boundary.

## v0.6.0 filesystem evidence contract

Scenario 36 adds the exact FAT16 proof without replacing any inherited fixture
or control. `docs/FAT16_QEMU_PROOF.md` records the complete 28-control matrix.
The live path reads BPB, first FAT, fixed root and cluster 2 from QEMU's
read-only regular-file namespace, requires four matching MSI-X completions,
validates `SAPOTE.BIN` and its documented 128-byte SHA-256 in Rust, and restores
the complete resource census before publishing its proof.

`.github/workflows/filesystem-milestone.yml` retains `make verify`, ten serial
TCG sweeps of all 36 scenarios, normalized logs, fixture layout/digests and an
explicit accelerator record. KVM remains unprobed because `/dev/kvm` is outside
the authorization boundary; safely exposed WHPX is verified separately from
the retained scenario ISOs.

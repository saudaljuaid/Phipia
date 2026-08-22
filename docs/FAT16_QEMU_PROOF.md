# QEMU FAT16/NVMe fixture and installed proof

Scenario 36, `filesystem`, is the only scenario that attaches the FAT16 image.
`tools/make-fat16-fixture.py` creates it as an ordinary temporary file under
`build/tests/filesystem/`; it does not call `mkfs`, mount, use a loop device or
inspect any host device. The generator reopens the file read-only and
independently checks its length, BPB fields and derived geometry, FAT entries,
root entry, payload bytes, unused bytes and digests before QEMU starts.

The file is exactly 16,777,216 bytes: 4096 sectors of 4096 bytes. BPB is at
LBA 0, the two-sector FAT begins at LBA 1, the 128-entry root occupies LBA 3,
and cluster 2/file data is LBA 4. These locations are fixture facts; the guest
derives and validates them from QEMU-written blocks.

The file payload SHA-256 is
`D399F065C9F21E2FD51E2AEADB7768EAB7E6E45E5150F31227C9711934A4D1D3`.
The complete image SHA-256 is
`B8FE53B80AAC718B36B545CC7A741ADCA52DF3BFE0DEE580D2A179B49DEBA5AC`.

## Attachment boundary

The Makefile uses QEMU's documented block graph, not `-drive`:

```text
-blockdev driver=file,filename=build/tests/filesystem/fat16-fixture.raw,node-name=filesystem-file,read-only=on,auto-read-only=off
-blockdev driver=raw,file=filesystem-file,node-name=filesystem-raw,read-only=on
-device nvme,drive=filesystem-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1,...
```

Both file and raw nodes are read-only, following the current
[QEMU invocation documentation](https://www.qemu.org/docs/master/system/invocation.html)
and [QEMU NVMe device documentation](https://www.qemu.org/docs/master/system/devices/nvme.html).
No passthrough device, host disk, host block node or host physical memory is
attached. QEMU's guest-visible PCI/NVMe emulation is the only device surface.
The scenario selects the test CD-ROM explicitly as its boot source, preventing
firmware from treating the signed superfloppy fixture as executable boot media.

The original `nvme` scenario retains `tools/make-nvme-fixture.py` and its
v0.5.0 LBA-8 pattern. While the FAT fixture is active, the raw NVMe proof emits
its declared neutral absence receipt rather than interpreting FAT metadata.

## Installed transcript

Only stable format and lifecycle facts appear in the public proof:

```text
ST FAT16 file SAPOTE.BIN bytes 128 reads 4 msix 4 ownership CPU-CONTROLLER-CPU teardown clean robustness 28
```

The transcript contains no address, path, timing, topology, controller serial
or volume identifier. The guest never calls an interrupt handler directly and
never substitutes compiled fixture bytes for PRP1 DMA data.

## Controlled robustness matrix

| # | controlled refusal or proof |
|---:|---|
| 1 | missing/wrong boot signature |
| 2 | truncation and namespace/FAT sector-size mismatch |
| 3 | unsupported sectors per cluster |
| 4 | invalid reserved-sector or FAT count |
| 5 | zero/conflicting/inconsistent total-sector variants |
| 6 | zero/conflicting/insufficient FAT-size values and checked arithmetic |
| 7 | zero, misaligned or wrong root geometry |
| 8 | overlapping, overflowing or out-of-namespace derived spans |
| 9 | derived FAT12 and FAT32 cluster counts |
| 10 | media mismatch and malformed FAT reserved entries |
| 11 | root scan without an in-sector end marker |
| 12 | absent target |
| 13 | duplicate canonical targets |
| 14 | LFN entry |
| 15 | deleted, label, directory or unsupported attribute |
| 16 | malformed/noncanonical 8.3 query or entry |
| 17 | cluster below two, wrong cluster or outside the data range |
| 18 | free, bad, reserved or out-of-range FAT entry |
| 19 | cyclic or other multi-cluster value instead of EOC |
| 20 | zero, wrong, oversized or destination-exceeding length |
| 21 | overflowing/out-of-range cluster translation |
| 22 | partial FFI output and allocated trailing state after root end |
| 23 | wrong byte count, guard, completion identity or read ordinal |
| 24 | inspection, parsing, copying, reuse or release without CPU ownership |
| 25 | cleanup at every filesystem and four-read/setup boundary |
| 26 | teardown race leaves released state unobservable |
| 27 | omitted or duplicated filesystem Boot Ledger prerequisite |
| 28 | temporary alternate filesystem scenario exit value |

Controls 1–22 use synthetic guest C byte arrays passed through the production
Rust ABI; C only constructs and mutates bytes and never parses them. The same
families also run as a host Rust unit test during `make verify`, without being
linked into the kernel. Controls 23–26 are guest-local typed lifecycle
controls. Controls 27–28 use temporary descriptor/exit values. None mutates the
live fixture. Every path ends with the same zero-resource census, and all
inherited NVMe controls still run.

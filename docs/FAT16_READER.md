# Bounded read-only FAT16 reader

Sapote v0.6.0 accepts one deliberately narrow FAT16 superfloppy. It is a
format parser, not a VFS: there is no mount table, partition discovery, path
walking, file descriptor, cache, write operation or application ABI. The only
query is the canonical root short name `SAPOTE  BIN`, returned to C as
`SAPOTE.BIN` only after Rust validates the entry.

## Normative format sources

The parser follows the FAT definition incorporated by
[UEFI 2.10 §13.3](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html)
and rechecked in the current
[UEFI 2.11 §13.3](https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html),
especially §13.3.1.1 (the BPB defines media, reserved space, FATs and the fixed
root) and §13.3.1.2 (8.3 names are uppercase ASCII). The byte-level reference
is Microsoft **FAT: General Overview of On-Disk Format**, version 1.03,
December 6, 2000, downloaded as `fatgen103.doc` from the
[official Microsoft legacy-document set](https://www.microsoft.com/en-us/download/details.aspx?id=53426).
UEFI 2.10 fixes that referenced format rather than allowing driver folklore to
evolve it. UEFI partition discovery in §13.3.2 is intentionally outside this
reader: namespace LBA zero is volume sector zero.

All multi-byte fields below are little-endian. Rust reads them from checked
byte ranges; no packed C or Rust structure overlays the disk block.

| FAT 1.03 field | byte offset | width | v0.6.0 rule |
|---|---:|---:|---|
| `BPB_BytsPerSec` | 11 | 2 | 4096 and equal to the identified NVMe LBA size |
| `BPB_SecPerClus` | 13 | 1 | 1 |
| `BPB_RsvdSecCnt` | 14 | 2 | 1 |
| `BPB_NumFATs` | 16 | 1 | 1 |
| `BPB_RootEntCnt` | 17 | 2 | 128 |
| `BPB_TotSec16` | 19 | 2 | 4096 |
| `BPB_Media` | 21 | 1 | `F8h` |
| `BPB_FATSz16` | 22 | 2 | 2 sectors |
| `BPB_HiddSec` | 28 | 4 | zero; there is no partition offset |
| `BPB_TotSec32` | 32 | 4 | zero; both total-size variants may not be populated |
| `BS_Reserved1` | 37 | 1 | zero |
| `BS_BootSig` | 38 | 1 | `29h` extended-BPB signature |
| boot signature | 510 | 2 | bytes `55h AAh` |

FAT 1.03's checked derivations are applied in this order:

```
RootDirSectors = (RootEntCnt * 32 + BytsPerSec - 1) / BytsPerSec
FATSz           = BPB_FATSz16
FirstFATSector  = BPB_RsvdSecCnt
FirstRootSector = FirstFATSector + BPB_NumFATs * FATSz
FirstDataSector = FirstRootSector + RootDirSectors
DataSectors     = TotalSectors - FirstDataSector
ClusterCount    = DataSectors / BPB_SecPerClus
```

Every addition, subtraction, multiplication, rounding step, division and
integer conversion is checked before the result is used. The accepted result
is one FAT at LBA 1–2, one root sector at LBA 3 and data beginning at LBA 4;
those values are conclusions and are then required by the proof contract, not
inputs to the derivation. All spans must be ordered, adjacent, non-overlapping,
inside 4096 volume sectors and inside the identified namespace.

FAT 1.03 classifies by `ClusterCount`, never by `BS_FilSysType`: fewer than
4085 clusters is FAT12, fewer than 65525 is FAT16, and all larger counts are
FAT32. This volume has 4092 clusters and is therefore FAT16.

## FAT and directory rules

FAT16 entries are 16-bit little-endian values. Entry zero must be `FFF8h`
(the `F8h` media byte followed by ones) and entry one `FFFFh`. A data entry of
`0000h` is free, `0001h` is reserved, `0002h`–`FFEFh` names another data
cluster, `FFF0h`–`FFF6h` is reserved, `FFF7h` is bad, and `FFF8h`–`FFFFh` is
end-of-chain. Cluster 2 must contain an EOC value; another data cluster would
be a fragmented or cyclic/multi-cluster file and is rejected.

The FAT 1.03 short directory entry is exactly 32 bytes:

| field | byte offset | width | v0.6.0 rule |
|---|---:|---:|---|
| `DIR_Name` | 0 | 11 | exact uppercase space-padded `SAPOTE  BIN` |
| `DIR_Attr` | 11 | 1 | exactly archive (`20h`) |
| `DIR_FstClusHI` | 20 | 2 | zero for FAT16 |
| `DIR_FstClusLO` | 26 | 2 | cluster 2 |
| `DIR_FileSize` | 28 | 4 | exactly 128 bytes |

`00h` in the first name byte is the bounded end marker and all later entries
must be zero. `E5h` is deleted. Attribute `0Fh` is an LFN entry; volume
(`08h`), directory (`10h`) and every attribute other than exact archive are
unsupported. An absent, duplicate, unrelated or malformed active entry fails
the whole exact-volume query rather than satisfying it accidentally.

The FAT 1.03 cluster translation is
`FirstDataSector + (cluster - 2) * SecPerClus`. Its checked result must contain
one full cluster in the validated data and namespace spans. The nonzero file
length must fit both the 4096-byte cluster and the 128-byte destination.

## Rust ABI contract

`src/rust/fat16.rs` is allocator-free `no_std` safe Rust. Unsafe operations are
confined to `src/rust/abi.rs`, where non-null C pointers and explicit lengths
become temporary slices and outputs are zeroed before any parse. The parser
retains no pointer and returns only `repr(C)` values: checked geometry, an
11-byte query, a copied root entry, copied FAT facts, a one-cluster extent and
a 32-byte SHA-256 result. Every failure leaves the corresponding output
invalid and all-zero.

The twenty-two parser controls cover the accepted layout and every metadata
refusal family. Controls 23–26 belong to the C ownership/lifecycle layer;
Boot Ledger and host-exit controls make the installed total 28.

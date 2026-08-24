<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Persistent FAT32

Sapote v2.0.0 has one bounded FAT32 implementation for two ordinary emulated
NVMe namespaces. It is a kernel-owned filesystem interface, not a POSIX layer,
general Unix VFS, or stable application ABI.

## Volumes and identity

The immutable system image is `sapote-system-fat32.raw`. It has volume ID
`0x20000001`, label `SAPOTESYS`, and contains `BUSYBOX`, `UNAMEBOX`, and
`CATBOX`. The mount is read-only below First Light. Each program still has an
independent filename, size, SHA-256, ELF64, and syscall contract. Historical
FAT16 images and their release evidence remain unchanged.

The writable data image is `sapote-data-fat32.raw`. It has volume ID
`0x20000002`, label `SAPOTEDATA`, and is First Light's user-data filesystem.
The two mounts have separate controller indices, generations, handles, cache
ownership, identity checks, and access policy. A missing or rejected data image
does not prevent the system image, kernel, or First Light from working.

## Deterministic geometry

Release images are 64 MiB superfloppies with the following exact geometry:

| Field | Value |
| --- | ---: |
| Logical sector | 512 bytes |
| Sectors per cluster | 1 |
| Reserved sectors | 32 |
| FAT copies | 2 |
| Sectors per FAT | 1,009 |
| Total sectors | 131,072 |
| Data clusters | 129,022 |
| Root cluster | 2 |
| FSInfo / backup boot | 1 / 6 |

Mount validates the primary and backup boot records, BPB and extended record,
checked sector/cluster arithmetic, FAT32 cluster count and capacity, volume
identity, both FSInfo sectors, and every sector of both FAT copies. FSInfo free
and next-free values are hints only; Sapote scans the FAT and replaces them
with measured values.

Before publishing a mount, Sapote walks the complete live tree. The walk checks
root and subdirectory chains, `.` and `..`, duplicate names, cycles,
cross-links, leaked clusters, bad/reserved/out-of-range values, exact file
size-to-chain length, and directory depth/count bounds. Irreconcilable media is
refused without exposing a partial mount.

## Names, paths, and resources

v2.0.0 deliberately accepts a case-insensitive ASCII 8.3 subset. Names are
uppercased on disk; supported punctuation includes dollar, percent, apostrophe,
hyphen, underscore, at, tilde, backtick, exclamation, parentheses, braces,
caret, hash, and ampersand. Long-filename entries are validated enough to
reject malformed ordinal
and non-ASCII UTF-16 data, then rejected as unsupported. This avoids presenting
partial VFAT semantics as complete support.

Paths are relative to one selected mount, at most 127 bytes and 16 components.
Empty, absolute, backslash-containing, repeated-separator, overlong, malformed,
and above-root paths are refused. Lookup never crosses to another mounted
volume. The root is an ordinary cluster chain. Each directory is limited to 64
live entries; the mount-time validator accepts at most 256 directories.

The kernel owns at most two mounts, sixteen generation-authenticated handles,
and four 512-byte cache entries. Handles retain access mode and offset. Stale,
double-closed, read-only, and cross-generation use is rejected. An open file
cannot be unlinked, renamed, or truncated. Files are bounded to 16 MiB.

## Kernel interface

`include/sapote/fat32_fs.h` exposes mount, unmount, sync, open, close, read,
write, seek, stat, list, create, truncate, mkdir, rename, unlink, and rmdir.
Reads at EOF succeed with a short or zero byte count. Writes report only bytes
successfully submitted; capacity, file-size, directory, and volume exhaustion
have named errors. Every C/Rust input range is validated before Rust creates a
slice, and invalid userspace copy ranges are rejected before any byte is moved.

## Allocation, cache, and persistence

Allocation scans from the checked next-free hint with a bounded wrap. A new
cluster is zeroed through NVMe before either FAT copy points to it. File data is
written before the file's size and first-cluster directory fields are updated.
Growth zeroes gaps; truncation zeroes the retained tail before releasing the
detached chain. Deletion marks the directory entry unreachable before freeing
its old chain. Failed allocation releases any newly owned chain.

FAT updates write the secondary copy first and the primary copy second. The
four-entry cache has one mount generation owner per entry and deterministic
least-stamp eviction. A dirty eviction must write back successfully; otherwise
the operation returns an error and does not replace the entry. Sync flushes the
cache and then writes primary and backup FSInfo. Unmount and controller reset
invalidate the mount generation and its cache and handles.

The persistence boundary is a successful `sync`, clean unmount, or clean
`reboot` command. Every completed operation uses synchronous NVMe writes, and
the clean boundary persists its final metadata hints. FAT32 is not journaled:
Sapote does not claim atomic multi-sector transactions or arbitrary power-loss
recovery. An interruption may leave secondary/primary FAT disagreement,
unreachable allocation, or a size/chain mismatch. The next mount detects and
refuses those states. Use `tools/fat32_image.py inspect` to diagnose the image;
v2.0.0 supplies inspection and deterministic reconstruction, not an in-kernel
repair service.

## Host tooling and verification

`tools/fat32_image.py` deterministically formats, populates, inspects, verifies,
and generates named malformed images. Inspection reports BPB/FSInfo geometry,
identities, directory trees, FAT-copy equality, cycles, cross-links, leaks,
invalid entries, and size mismatches. `tools/fat32_host_test.py` exercises the
positive and adversarial host contracts.

```sh
make fat32-images
python3 tools/fat32_image.py inspect build/userspace/sapote-data-fat32.raw
python3 tools/fat32_image.py verify data build/userspace/sapote-data-fat32.raw
make verify
make qemu-tests
```

All production evidence uses the normal NVMe submission/completion path and
First Light commands. Host inspection is verification tooling, not a substitute
for guest filesystem execution.

## Retained limits

There is no journal, fsck repair mode, POSIX metadata, permissions model,
timestamps, hard links, symbolic links, sparse files, multi-user security,
arbitrary FAT32 geometry, VFAT long-name support, hotplug, concurrent writers,
or stable userspace ABI. These are explicit v2.0.0 boundaries.

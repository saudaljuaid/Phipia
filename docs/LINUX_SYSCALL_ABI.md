<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Measured Linux x86_64 syscall boundary

Sapote runs two pinned static BusyBox programs as bounded compatibility proofs:

- v0.8.0: `busybox echo SAPOTE`;
- v0.9.0: `busybox uname -s`.

This is not POSIX, a native Sapote ABI, or a general Linux personality. Each
profile has a distinct executable, configuration, FAT16 fixture, initial stack,
syscall allowlist, output sink, lifecycle, and checksum.

## Entry and return

The kernel programs and reads back `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR`,
and `IA32_FMASK`. On `syscall`, assembly saves the user stack, closes the
interrupt window, switches to the validated TSS kernel stack, and calls C only
after authenticating the active process, generation, private CR3, CPL3 entry,
and executable range.

The syscall number is in `RAX`; arguments use `RDI`, `RSI`, `RDX`, `R10`, `R8`,
and `R9`; the signed result returns in `RAX`. Refusals use negative Linux errno
values. Return uses a checked `iretq` after validating the complete user frame
and lifecycle state. `int 0x80`, the native proof gate, direct handler calls,
and `sysretq` cannot satisfy this boundary.

## Echo profile

The committed trace is `userspace/busybox/syscall-allowlist.txt`.

| Number | Call | Accepted operation |
| ---: | --- | --- |
| 1 | `write` | fd 1, exactly `SAPOTE\n`, once |
| 9 | `mmap` | the measured anonymous guard and RW page only |
| 11 | `munmap` | the preceding measured RW page only |
| 12 | `brk` | fixed-base query and one exact 8192-byte growth |
| 158 | `arch_prctl` | measured `ARCH_SET_FS` address only |
| 218 | `set_tid_address` | measured writable address only |
| 231 | `exit_group` | status zero only |

All other numbers return `-ENOSYS` without widening process state.

## Uname profile

The normalized sequence in
`userspace/busybox/uname-syscall-sequence.txt` is:

```text
arch_prctl
set_tid_address
uname
ioctl
writev
exit_group
```

The exact arguments are pinned in
`userspace/busybox/uname-syscall-allowlist.txt`. `ioctl` is only the measured fd
1 `TIOCGWINSZ` probe returning `-ENOTTY`; `writev` accepts only the measured
two-element `Linux\n` output. Neither creates a terminal, descriptor table, or
general vector-I/O service.

Linux x86_64 syscall 63 writes one complete 390-byte `new_utsname` record:

| Field | Offset | Width |
| --- | ---: | ---: |
| `sysname` | 0 | 65 |
| `nodename` | 65 | 65 |
| `release` | 130 | 65 |
| `version` | 195 | 65 |
| `machine` | 260 | 65 |
| `domainname` | 325 | 65 |

The whole destination must be canonical, mapped, user-owned, and RW/NX before
any byte changes. Null, wrapped, supervisor, executable, read-only, unmapped,
MMIO, DMA, page-table, guard, foreign, and cross-resource ranges return
`-EFAULT` with no partial output. The record is immutable and Sapote-owned;
there is no hostname mutation or UTS namespace.

## Image, stack, and storage limits

Both programs are static, position-fixed x86_64 `ET_EXEC` images parsed by
Rust before allocation or mapping. Interpreter, dynamic, relocation, PIE,
executable-stack, and W+X shapes are refused.

Each profile receives exactly three arguments, an empty environment, and the
measured `AT_PAGESZ`/`AT_NULL` auxiliary vector in a guarded RW/NX stack. Each
fixture is a separate read-only 16 MiB FAT16 image attached to an emulated NVMe
namespace. DMA ownership returns to the CPU before Rust inspects file bytes.

Checksums, source provenance, and reproducible build instructions are in
[`BUSYBOX_REPRODUCIBLE_BUILD.md`](BUSYBOX_REPRODUCIBLE_BUILD.md).

## Deliberate limits

There are no paths, writable files, signals, multiple processes, dynamic
linking, PIE, sockets, native Sapote syscalls, general mappings, general
descriptors, hostname mutation, `int 0x80`, or stable compatibility promise.
Adding another program means measuring and pinning a new profile.

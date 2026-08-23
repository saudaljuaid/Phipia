<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded Linux x86-64 uname ABI

Sapote implements one measured `uname` operation for the v0.9.0
`busybox uname -s` proof. It is not a general POSIX layer, public identity
service, UTS namespace, or hostname-management interface.

## Authoritative and measured shape

The Linux x86-64 syscall table assigns `uname` number 63. The Linux UAPI
defines `__NEW_UTS_LEN` as 64 and lays out six arrays of
`__NEW_UTS_LEN + 1` bytes. The authoritative references are the Linux kernel
[`syscall_64.tbl`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/x86/entry/syscalls/syscall_64.tbl)
and [`include/uapi/linux/utsname.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/utsname.h).

`tools/linux-uname-abi-measure.c` independently compiles against the Ubuntu
runner libc and the freshly built pinned musl 1.2.6. Both measurements must
agree with these committed facts:

| Field | Value |
| --- | ---: |
| Syscall number | 63 |
| Argument/result convention | pointer in `RDI`; zero or negative Linux errno in `RAX` |
| Structure size/alignment | 390 bytes / 1 byte |
| `sysname` | offset 0, 65 bytes |
| `nodename` | offset 65, 65 bytes |
| `release` | offset 130, 65 bytes |
| `version` | offset 195, 65 bytes |
| `machine` | offset 260, 65 bytes |
| `domainname` | offset 325, 65 bytes |

The observed six-call sequence is 158, 218, 63, 16, 20, 231. Only those six
numbers are allowlisted for this process profile. Every other syscall returns
`-ENOSYS` without advancing process state.

## Entry and completion

The unmodified image executes a real x86-64 `syscall` instruction at CPL3.
The inherited `IA32_LSTAR` entry authenticates the active process, generation,
private CR3, executable interval, saved user stack, register frame, and syscall
provenance before dispatch. Sapote never runs kernel C on the user stack and
returns through the checked `IRETQ` boundary.

For syscall 63 the complete destination range must pass the checked-copy-out
contract before any byte changes. A valid call installs the complete
Sapote-owned record and returns zero. Invalid destinations return `-EFAULT`
without a partial write. No `sethostname`, `setdomainname`, per-process UTS
state, host identity, or public read API exists.

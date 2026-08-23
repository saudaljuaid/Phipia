<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Deterministic Sapote UTS record

The v0.9.0 uname proof owns one immutable 390-byte record:

| Field | Sapote-owned value | Meaning |
| --- | --- | --- |
| `sysname` | `Linux` | Required compatibility identity for the measured applet. |
| `nodename` | `sapote` | Fixed proof-local name; not a host name. |
| `release` | `0.9.0-sapote` | Milestone-specific compatibility label; not a Linux kernel release claim. |
| `version` | `Sapote` | Fixed project identity; contains no build or toolchain data. |
| `machine` | `x86_64` | Truthful architecture of the selected executable ABI. |
| `domainname` | `(none)` | Explicit fixed absence; not a configured network domain. |

Every field is a 65-byte array. The text is NUL-terminated and every trailing
byte is zero. Compile-time offset, size, and alignment assertions are repeated
by runtime semantic controls. The record contains no host hostname, domain,
kernel release, timestamp, environment, address, process generation, path, or
toolchain identity.

The record is private to the one `linux-abi-uname` proof. It is created before
Ring 3 entry, may be copied out exactly once through authenticated syscall 63,
and is released before the result is published. There is no mutation operation
and no public UTS namespace or hostname service.

<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Checked userspace copy-out

The uname proof adds one private, fixed-length copy-out operation. Its only
payload is the 390-byte deterministic UTS record, and its only authorized
caller is the authenticated uname syscall for the active process generation
and private CR3.

## Validate, then modify

Before copying, the implementation rejects zero length, null, wrapped, or
noncanonical ranges. It walks every covered page and requires the active
process to own each page as writable and non-executable user memory. Kernel,
MMIO, DMA, page-table, filesystem, guard, RX, read-only, unmapped, and
cross-resource destinations therefore fail before the first store.

Validation covers the entire half-open range `[destination, destination +
390)`. Only after that pass succeeds does the copy loop install all bytes. The
operation consequently supports a UTS record crossing two valid RW/NX pages,
but a range whose later page is invalid returns `-EFAULT` and preserves the
earlier valid bytes. Controlled sentinels prove the non-partial guarantee.

## Typed ownership

The copy state is `candidate -> active -> completed -> released` on success or
`candidate -> active -> failed -> released` on refusal. Repeated, reversed,
stale-generation, foreign-process, foreign-CR3, post-exit, and post-release
transitions return named failures. Result publication requires released copy,
stdout, syscall, process, mapping, filesystem, and DMA state plus the exact
pre-proof resource census.

This seam is not a general `copy_to_user`, writable mapping, or kernel-output
facility. Its fixed record, size, provenance, and lifecycle are part of the
v0.9.0 proof contract.

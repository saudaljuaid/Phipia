# Linux uname QEMU proof and controlled robustness

## Scope and sequencing

This matrix was committed before the installed v0.9.0 proof was implemented.
It is the acceptance boundary for one invocation of the separately configured,
checksum-pinned `busybox uname -s` image. The inherited v0.8.0
`busybox echo SAPOTE` image, configuration, fixture, transcript, and digest are
independent contracts and cannot satisfy any uname result.

The positive result is exactly `Linux\n`, exit status zero, execution at CPL3
through the existing real x86-64 `SYSCALL`/`IA32_LSTAR` boundary, complete
390-byte checked UTS copy-out, and restoration of the pre-proof resource census.
Every negative control below is a successful harness assertion that a named
failure was returned with state and memory unchanged; an expected rejection is
never represented by a red CI job.

## Controlled robustness matrix

| # | Controlled input or transition | Required assertion |
| ---: | --- | --- |
| 1 | Truncated ELF header or any newly accepted program-header prefix | Safe Rust rejects before allocation or mapping. |
| 2 | More than eight headers, four `PT_LOAD` segments, 512 clusters, 2 MiB, three arguments, measured auxiliary entries, or sixteen syscall numbers | The relevant bounded parser or builder rejects without widening its count. |
| 3 | `ET_DYN`, interpreter, dynamic header, relocation, constructor dependency, executable stack, or W+X segment | Image validation rejects before executable ownership exists. |
| 4 | Overlapping, wrapped, noncanonical, misaligned, invalid-BSS, invalid-page-rounding, or entry-outside-RX segments | Image validation rejects with no user mapping. |
| 5 | FAT cycle, free/reserved/bad cluster, premature/late EOC, overlong chain, out-of-volume extent, or arithmetic overflow | The separate uname read session rejects and returns filesystem ownership to the CPU. |
| 6 | Wrong `argc`, `argv`, `envp`, auxiliary vector, entry alignment, string bound, or stack pointer | Stack construction/validation rejects before Ring 3 entry. |
| 7 | Wrong `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_EFER.SCE`, selectors, CR3, process, generation, or provenance token | Syscall CPU/process authentication rejects before dispatch. |
| 8 | Entry from CPL0, `int 0x80`, inherited `int 0x81`, or direct handler call | No uname request/result or stdout proof can become valid. |
| 9 | Kernel C execution attempted while the user RSP is live | Entry control rejects; the validated kernel/TSS stack remains mandatory. |
| 10 | Syscall number outside the measured allowlist | Return is exactly `-ENOSYS`; lifecycle and census are unchanged. |
| 11 | Wrong argument register, result encoding, RCX/R11 state, or IRET frame | Syscall/return validation rejects with a named failure. |
| 12 | Null uname pointer | Return is exactly `-EFAULT`; no user byte changes. |
| 13 | Noncanonical, wrapped, unmapped, kernel, MMIO, DMA, page-table, guard, RX, read-only, or cross-resource uname destination | Full-range validation returns `-EFAULT` before copying. |
| 14 | Complete 390-byte UTS destination spanning two valid RW/NX pages | Copy-out succeeds once and both page portions are exact. |
| 15 | Destination begins valid but crosses into an invalid page/resource | Return is `-EFAULT`; every destination byte retains its sentinel. |
| 16 | A later page fails after an earlier page validates | No partial copy occurs and copy-out state releases cleanly. |
| 17 | Injected failure at each validation checkpoint before the copy loop | No destination byte or process register changes except the defined `-EFAULT` result. |
| 18 | UTS field reorder, non-65-byte field, missing NUL, wrong 390-byte size, or alignment mismatch | Compile-time and runtime ABI layout controls reject. |
| 19 | Host-derived or nondeterministic sysname, node, release, version, machine, or domain | Identity validation rejects; only the documented Sapote-owned record is accepted. |
| 20 | Wrong descriptor, bytes, length, partial/repeated write, write after exit, or foreign process/generation/CR3/provenance | Uname stdout sink becomes invalid and cannot publish success. |
| 21 | Allocation failure at every executable, hierarchy, stack, optional heap, fixture, syscall, UTS, and stdout boundary | Reverse-order teardown restores the exact pre-proof census. |
| 22 | Failure immediately before and after every observed syscall | Process stops once, releases all syscall state, and restores kernel CR3. |
| 23 | Duplicate exit, syscall after exit, return after disarm, stale return, reversed transition, timeout, or teardown race | Typed lifecycle returns a named failure; no result publishes. |
| 24 | Missing/duplicate Boot Ledger prerequisite, result capability, or success/neutral cardinality | Ledger validation fails the stage and never substitutes another scenario. |
| 25 | Alternate scenario name, guest exit, or host exit | Source and host contracts accept only `linux-abi-uname`, `0x37`, and `111`. |
| 26 | Every positive and negative control, including inherited echo preservation | Kernel CR3, filesystem/DMA ownership, mappings, process/syscall owners, and the complete resource census equal their pre-control values. |

## Measured contract gate

Kernel implementation remains gated on the `Linux uname BusyBox contract`
workflow producing two byte-identical clean builds, a static position-fixed
x86-64 `ET_EXEC` no larger than 2 MiB, at most eight program headers/four load
segments/512 FAT16 clusters, no interpreter/dynamic dependency/relocation/RWX
segment, no exercised floating-point or vector instruction, exactly
`Linux\n`, exit zero, and a trace with no more than sixteen distinct syscall
numbers. The trace must add a non-empty semantic delta to v0.8.0; the expected
delta is Linux x86-64 syscall 63 (`uname`), but the captured trace is
authoritative.

The first measurement established the bounded sequence `arch_prctl`,
`set_tid_address`, `uname`, `ioctl`, `writev`, `exit_group`. The `ioctl` is only
the exact descriptor-1 `TIOCGWINSZ` probe returning `-ENOTTY`; it does not add a
terminal. The `writev` is only the exact two-element vector `"Linux"`, `"\n"`.
Both are private proof operations, not public descriptor or vector-I/O ABIs.

The same workflow compiles `tools/linux-uname-abi-measure.c` against both the
runner libc and the freshly built pinned musl. Their exact syscall number,
structure size, alignment, field order, offsets, and 65-byte field widths must
match before the digest is pinned or implementation begins.

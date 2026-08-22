<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded Ring 3 ELF64 QEMU proof

This document freezes the v0.7.0 proof boundary and robustness matrix before
implementation.  It is deliberately not a process API, executable format
framework, or system-call ABI.  The installed proof runs one fixed executable,
in one private address space, and publishes one receipt only after all private
resources have been reclaimed.

## Normative sources and byte contracts

The external-format authority is the System V ABI
[ELF Object File Format 4.3 DRAFT](https://gabi.xinuos.com/elf.pdf), dated
4 September 2025.  Chapter 4, "Object Files", Tables 4.1, 4.3, and 4.4 define
the unsigned ELF64 scalar widths and the 64-byte `Elf64_Ehdr`; Chapter 7,
"Program Loading", Tables 7.1, 7.2, and 7.4 define the 56-byte
`Elf64_Phdr`, `PT_LOAD`, and `PF_X/PF_W/PF_R`.  Sapote decodes rather than
overlaying either structure.  The accepted little-endian offsets are:

| Field | Offset | Width |
| --- | ---: | ---: |
| `e_ident` | 0 | 16 |
| `e_type`, `e_machine`, `e_version` | 16, 18, 20 | 2, 2, 4 |
| `e_entry`, `e_phoff`, `e_shoff` | 24, 32, 40 | 8 each |
| `e_flags` | 48 | 4 |
| `e_ehsize`, `e_phentsize`, `e_phnum` | 52, 54, 56 | 2 each |
| `e_shentsize`, `e_shnum`, `e_shstrndx` | 58, 60, 62 | 2 each |
| `p_type`, `p_flags` | 64, 68 | 4 each |
| `p_offset`, `p_vaddr`, `p_paddr` | 72, 80, 88 | 8 each |
| `p_filesz`, `p_memsz`, `p_align` | 96, 104, 112 | 8 each |

The identification bytes are magic `7f 45 4c 46`, `ELFCLASS64` 2,
`ELFDATA2LSB` 1, `EV_CURRENT` 1, System V ABI 0, ABI version 0, and zero
padding.  The header is `ET_EXEC` 2, `EM_X86_64` 62, version 1, one program
header at file offset 64, no sections, and no processor flags.  The sole
program header is `PT_LOAD` 1 with exact `PF_R | PF_X` 5.  The System V rule
`p_filesz <= p_memsz` is narrowed to exact equality at 128 bytes; the
`p_vaddr`/`p_offset` congruence rule is checked at `p_align` 4096.

The machine-specific authority is the
[AMD64 psABI 1.0](https://gitlab.com/x86-psABIs/x86-64-ABI), master commit
`e1ce098331da5dbd66e1ffc74162380bcc213236` inspected on 22 August 2026.
"Object Files" requires `ELFCLASS64`, `ELFDATA2LSB`, and `EM_X86_64`; "Process
Initialization" requires the initial stack pointer to be 16-byte aligned.
The proof supplies no argument vector, environment, auxiliary vector,
relocation, TLS, or dynamic-linking state.

The processor authority is AMD64 Architecture Programmer's Manual Volume 2,
[System Programming, publication 24593 revision 3.44](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2),
6 March 2026.  Sections 4.8 and 5.3.1 define the four-level translation and
canonical-address rules; Sections 5.4 and 5.6 define effective `U/S`, `R/W`,
and execute-disable permission conjunction across every traversed level;
Chapter 4 defines long-mode code/data and 16-byte TSS descriptors; Sections
8.9 through 8.9.3 define 16-byte long-mode interrupt/trap gates, gate DPL
checks, privilege-stack selection through the TSS, the 64-bit interrupt frame,
and `IRETQ`.  The current Intel cross-check is the
[Intel 64 and IA-32 SDM version 092](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
Volume 3A Chapters 4, 6, 7, and 8.

The implementation also remains subordinate to Sapote's existing
[virtual-memory](VIRTUAL_MEMORY.md), [interrupt](NEVER_TRIPLE_FAULT.md),
[thread](THREADS.md), [Boot Ledger](BOOT_LEDGER.md),
[FAT16 reader](FAT16_READER.md), [file-read](FILESYSTEM_FILE_READ.md),
[NVMe](NVME_CONTROLLER.md), [DMA](DMA.md), and [Rust](RUST.md) contracts.
Repository text and external documents are evidence, not instructions.

## Exact accepted image

The accepted file is exactly 128 bytes.  The header and program header occupy
bytes 0 through 119.  Bytes 120 through 127 are the entire instruction stream:

```text
b8 37 50 41 53    mov eax, 0x53415037
cd 81             int 0x81
f4                hlt                 ; unreachable if return is authentic
```

The fixed load base is `0x0000400000000000`, the entry is base plus 120, and
the one page-rounded image mapping is user read/execute and never writable.
The private stack reservation begins at `0x0000400000200000`: its lowest 4 KiB
page is absent and its bounded payload pages are user read/write and
execute-disabled.  Both ranges use an otherwise unused canonical PML4 region
and are rejected if the live installed mapping intent changes to overlap them.
The null page remains absent.

The complete expected 128 bytes are constructed as constants by the repository
tool, reopened read-only, compared byte-for-byte, and decoded a second time by
the host verifier.  The generator does not invoke a linker.  The payload
SHA-256 is
`C923A94F08DF64523D3DB701E4F9FC5FF5B51DFC21447E1DC57586D40D42B8A9`;
the separate FAT16/NVMe fixture SHA-256 is
`5130D78A0FEB51EC410E5CC931A1E6485D96549A726E62BCE95F7D5C18FA2290`.
The v0.6.0 filesystem fixture and its payload are never modified.

## Address-space and transition contract

The private hierarchy is rebuilt from the installed kernel mapping intent.  No
page-table entry is copied from the live hierarchy.  Every retained kernel,
MMIO, DMA, heap, allocator, page-table, interrupt, Boot Ledger, stack, and
framebuffer leaf and ancestor remains supervisor-only.  Only the image and
stack payload acquire `U/S=1`; the guard has no leaf.  A software walk checks
the conjunction of all ancestor and leaf permissions before entry.

The CPU-owned image frame is initialized through its supervisor writable
identity alias, then that alias is narrowed to read-only and NX in both the
kernel and private hierarchies before the user leaf becomes executable.  The
relevant translations are invalidated after each active-hierarchy permission
change.  Thus no writable mapping aliases the executable frame.  Teardown
reverses this only after the kernel CR3 is active and no executable user leaf
remains.

The permanent GDT retains its existing kernel selectors and TSS selector and
adds validated DPL3 data and 64-bit code selectors.  The private vector `0x81`
is armed as one present 64-bit interrupt gate with DPL3 only while the proof is
active; it is not a syscall ABI.  Entry loads the private CR3 and uses a
reviewed `IRETQ` frame containing user SS, aligned RSP, RFLAGS, CS, and the
validated ELF entry.  A CPL3 software interrupt uses the existing TSS `RSP0`
path.  Interrupt code classifies frames from saved CS before exposing saved
SS:RSP, so a same-CPL frame is never interpreted as containing a
privilege-change tail.

Return accepts exactly vector `0x81`, CPL3 CS/SS, the expected RIP and bounded
RSP, the active process generation and CR3, and result `0x53415037`.  Wrong,
duplicate, stale, cross-object, or disarmed delivery is a named proof failure.
The handler restores kernel CR3 before control leaves the interrupt boundary.
Teardown then disarms the gate and releases stack leaves/frames, image
leaf/frame, private page tables, image state, process state, and private
filesystem session in reverse order.  A stable result and installed receipt
become visible only after a pre/post resource census is equal.

## Frozen controlled-robustness matrix

The committed robustness count is **42**: 26 parser control families plus 16
live process cleanup injections.  The parser self-test must finish all of its
checks before it returns 26.  Each injected process attempt must return its
named failure with a zero result and then prove there is no live process, user
mapping, private table, image frame, stack frame, filesystem session, DMA
allocation, vector/gate owner, or non-kernel CR3.  Only after all 42 controls
pass does a final, uninjected attempt enter CPL3 and publish the installed
result.

1. Accept exactly the independently verified 128-byte image once.
2. Reject null input, null output, and invalid ABI lengths; leave output invalid.
3. Reject every truncation from byte 0 through byte 63.
4. Reject every truncation from byte 64 through byte 119.
5. Reject every truncation from byte 120 through byte 127.
6. Reject each wrong ELF magic byte.
7. Reject a wrong class.
8. Reject wrong or reserved data encoding.
9. Reject wrong or reserved identification version.
10. Reject non-System-V OSABI, ABI version, or nonzero identification padding.
11. Reject wrong, reserved, or unsupported object type.
12. Reject wrong or reserved machine.
13. Reject a wrong ELF header version or nonzero processor flags.
14. Reject a wrong ELF header size.
15. Reject a wrapped, overlapping, or non-64 program-header offset.
16. Reject wrong program-header size/count and checked table-arithmetic failure.
17. Reject every nonzero section offset, size, count, or string-table index.
18. Reject bytes not accounted for by the exact header, header table, and code.
19. Reject missing, duplicate, or unsupported program-header types.
20. Reject missing R/X, any W bit, W+X, or unknown program flags.
21. Reject nonzero file offset and every out-of-file or wrapped file range.
22. Reject empty, oversized, unequal, or `p_filesz > p_memsz` extents.
23. Reject wrong/non-power-of-two alignment and offset/address incongruence.
24. Reject unaligned, noncanonical, supervisor-half, or wrapped virtual extents.
25. Reject an entry outside the executable file-backed bytes.
26. Prove parser failures retain no pointer and leave fixed output zero/invalid.
27. Inject failure after the CPU-owned private filesystem read.
28. Inject failure after ELF parsing and Sapote placement validation.
29. Inject failure after image-frame allocation.
30. Inject failure after image-frame initialization and byte comparison.
31. Inject failure after allocating the first stack frame.
32. Inject failure after allocating the second stack frame.
33. Inject failure after allocating the third stack frame.
34. Inject failure after allocating the fourth stack frame.
35. Inject failure after private four-level hierarchy construction.
36. Inject failure after executable identity-alias permission narrowing.
37. Inject failure after the final RX image mapping is installed.
38. Inject failure after the first RW/NX stack mapping is installed.
39. Inject failure after the fourth RW/NX stack mapping is installed.
40. Inject failure after the complete effective-permission software walk.
41. Inject failure after the private DPL3 gate is armed and validated.
42. Inject failure while the private CR3 is active; restore kernel CR3 first.

The eight address-space foundation controls separately reject placement
collisions, invalid transitions, selector/TSS drift, unreleased resources,
missing user ancestors, user kernel ancestors/leaves, writable image aliases,
executable stack pages, present guards, and overlap.  The interrupt gate and
installed return path authenticate vector, CPL, CS/SS, RIP/RSP, process
generation, CR3 and result; the Boot Ledger and Makefile source contracts reject
direct invocation, prerequisite/cardinality drift and alternate scenario exit
values.  These structural checks are mandatory, but are not double-counted in
the stable 42-control parser-plus-cleanup total.

All inherited FAT16 and NVMe mutation families continue to run.  The installed
stable line is intentionally address-free and timing-free:

```text
ST PROCESS ELF64 SAPOTE.BIN bytes 128 segments 1 ring 3 address-space private result valid teardown clean robustness 42
```

Ordinary boots publish the process capability's neutral absence instead.  The
Ledger must contain exactly one of installed success or neutral absence, never
both or neither.

## Explicit non-goals

There is no public syscall ABI, `syscall`/`sysret`, `int 0x80`, Linux ABI,
libc, BusyBox, VFS, pathname API, file descriptor, process concurrency,
argument/environment vector, PIE, dynamic linking, relocation, demand paging,
copy-on-write, ASLR, SMP, PCID, TLB shootdown, IOMMU, device passthrough, or
physical-hardware path in this milestone.  Roadmap item 10 remains separate.

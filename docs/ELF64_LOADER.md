<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded ELF64 parser and loader

Sapote v0.7.0 accepts one executable format shape, not ELF in general. The
input is exactly 128 bytes, is parsed by safe `no_std` Rust, and produces one
pointer-free `repr(C)` description. C applies the installed Sapote placement
policy, copies all 128 bytes into a CPU-owned frame, narrows every writable
alias, and only then installs the final user RX mapping.

The format authority is the System V ABI [ELF Object File Format 4.3
DRAFT](https://gabi.xinuos.com/elf.pdf), 4 September 2025: Chapter 2 section
2.1, Listing 2.1 and Tables 2.1 through 2.6 define the ELF header,
identification, type, version and encoding fields; Chapter 7 sections 7.1,
7.2 and 7.4, Listing 7.1 and Tables 7.1 through 7.3 define the program header,
`PT_LOAD` and `PF_R/PF_W/PF_X`. The AMD64 convention cross-check is the
[AMD64 psABI 1.0](https://gitlab.com/x86-psABIs/x86-64-ABI) at master commit
`e1ce098331da5dbd66e1ffc74162380bcc213236`. Together they define the 64-byte
ELF64 header, 56-byte program header, little-endian scalar widths, `ET_EXEC`,
`EM_X86_64`, `PT_LOAD`, and flag values. Sapote never casts caller bytes to a
packed C or Rust structure.

## Accepted byte contract

- `e_ident`: `7f 45 4c 46`, class 2, data 1, version 1, OSABI and ABI version
  zero, and seven zero padding bytes;
- `e_type=2`, `e_machine=62`, `e_version=1`, `e_flags=0`;
- `e_ehsize=64`, `e_phoff=64`, `e_phentsize=56`, `e_phnum=1`;
- `e_shoff=e_shentsize=e_shnum=e_shstrndx=0`;
- one `PT_LOAD` with flags exactly `PF_R|PF_X=5`;
- `p_offset=0`, `p_vaddr=0x0000400000000000`, `p_paddr=0`;
- `p_filesz=p_memsz=128`, `p_align=4096`; and
- entry `0x0000400000000078`, inside the file-backed executable bytes.

The parser rejects truncation before each read. Its scalar helpers use checked
little-endian decoding; program-table multiplication/addition, file ranges,
virtual ends, page rounding and integer conversions are checked separately.
Alignment must be a power of two and the System V `p_vaddr`/`p_offset`
congruence must hold. The virtual range must remain in the lower canonical
half. There is no section-table dependency, interpreter, dynamic segment,
relocation, symbol, TLS, note, shared library, zero-fill tail, or second
segment.

Bytes 120 through 127 are the complete text:

```text
b8 37 50 41 53    mov eax, 0x53415037
cd 81             int 0x81
f4                hlt
```

## Rust/C boundary

`src/rust/elf64.rs` contains only safe parsing. `src/rust/abi.rs` validates the
input pointer, output pointer and exact length, zeroes the output, constructs
one temporary slice in the reviewed unsafe boundary, calls the safe parser,
and copies the fixed result. No caller pointer is retained or returned. Every
failure leaves `valid=0` and all other output bytes zero.

The fixed output is 88 bytes: validated identifiers and flags, entry, file and
virtual extents, page-rounded mapping limits, and an eight-byte copy of the
instruction stream. Status values name each rejected field family. The parser
uses no allocator, panic path, floating point, MMX, SSE, or AVX. Host tests and
the guest ABI self-test exercise all truncations and every accepted/rejected
family; `make verify` also rejects unsafe Rust outside `abi.rs`.

## Loader lifecycle

The image moves through candidate, validated, extent-checked, frame-allocated,
initialized, mapped, executable, and reclaimed states. C does not decode ELF
again. It checks that the validated result equals the one installed virtual
layout, allocates one frame, zeroes it, copies and compares all 128 bytes, then
asks paging to narrow the frame's identity alias to supervisor read-only/NX.
The user leaf is installed RX only after that narrowing and a CR3-backed walk.

The page contains the header as well as text because `p_offset=0`. Execution
starts at byte 120; no compiled-in kernel copy is a substitute. Teardown first
removes the user executable leaf, then restores the supervisor alias and frees
the private hierarchy and image frame. See [PROCESS_ADDRESS_SPACE.md](PROCESS_ADDRESS_SPACE.md)
and [CPL3_INTERRUPT_BOUNDARY.md](CPL3_INTERRUPT_BOUNDARY.md).

## Deferred work

PIE/`ET_DYN`, more segments, writable data, BSS, relocations, dynamic linking,
interpreters, symbols, TLS, arguments, environment, auxiliary vectors and a
public executable API are outside v0.7.0. The later Linux ABI milestone must
design them rather than widening this proof parser implicitly.

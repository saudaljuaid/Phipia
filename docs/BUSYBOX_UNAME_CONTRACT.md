<!-- SPDX-License-Identifier: GPL-3.0-only -->

# BusyBox uname provenance, configuration, and trace

The v0.9.0 userspace work is a separate GPL-2.0 BusyBox work linked against
separately licensed musl. No BusyBox, musl, or Linux implementation code is
copied into Sapote's GPL-3.0-only kernel.

## Reproducible inputs and result

| Item | Committed contract |
| --- | --- |
| BusyBox | 1.38.0 archive, SHA-256 `34F9EA6FF8636F2C9241153B9114EEFA9E65674A45318AE1EF95BB5F31C53BB2` |
| musl | 1.2.6 archive, SHA-256 `D585FD3B613C66151FC3249E8ED44F77020CB5E6C1E635A616D3F9F82460512A` |
| Configuration | `userspace/busybox/busybox-uname.config`, SHA-256 `6D972C7A1F3DF0034D5996CC24B58B7364EFBB7851F926C5D8D2FD18C41EBB2B` |
| Executable | 38,368 bytes, SHA-256 `389AD6B13804EB7307BA589C8E8A7C702F91302005A7C5FC6E9E99124FCEAF43` |

`tools/build-busybox-uname-proof.sh` extracts clean source directories twice,
verifies both source archives and the configuration, and requires byte-identical
executables. The BusyBox tree is unmodified. A build-only forced header applies
GCC's scalar target attribute only to musl `vfprintf`; this keeps the exercised
integer-only `uname -s` formatting path free of MMX/SSE/AVX while leaving both
published source archives byte-identical.

The result is a position-fixed x86-64 `ET_EXEC` at entry
`0x40000100107A`, with five program headers and four `PT_LOAD` segments:

| Offset | Virtual address | File/memory bytes | Permissions |
| ---: | ---: | ---: | --- |
| `0x0` | `0x400001000000` | `0x158 / 0x158` | R |
| `0x1000` | `0x400001001000` | `0x6D7F / 0x6D7F` | R-X |
| `0x8000` | `0x400001008000` | `0x1181 / 0x1181` | R |
| `0x91A0` | `0x40000100A1A0` | `0x20E / 0xC70` | RW- |

The fifth header is a non-executable RW GNU stack. There is no interpreter,
dynamic dependency, runtime relocation, PIE, shared object, or RWX segment.

## Independent trace

The normalized sequence in `userspace/busybox/uname-syscall-sequence.txt` is:

```text
arch_prctl
set_tid_address
uname
ioctl
writev
exit_group
```

The exact allowlist and arguments are recorded in
`userspace/busybox/uname-syscall-allowlist.txt`. The semantic delta from v0.8.0
is syscall 63 (`uname`). The invocation prints exactly the six bytes `Linux\n`
and exits zero.

QEMU user-mode emits translated block addresses and bytes, not disassembly
mnemonics. `tools/check-exercised-instructions.py` therefore joins those ranges
to the complete `objdump` listing and rejects any overlapping floating-point,
MMX, SSE, or AVX instruction. Its self-test proves both overlap rejection and
unexecuted-instruction tolerance. The measured uname and inherited echo traces
both pass this audit.

The source archives, configuration, BusyBox license, musl copyright record,
build record, syscall evidence, and checksums remain release materials beside
the binary-containing ISO.

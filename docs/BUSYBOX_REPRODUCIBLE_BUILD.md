<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Reproducible BusyBox proof inputs

BusyBox and musl are separate userspace works. Neither is copied into or linked
with Sapote's GPL-3.0-only kernel. Their source, configurations, licenses,
traces, and build records remain distinct release materials.

## Pinned source inputs

| Input | SHA-256 |
| --- | --- |
| BusyBox 1.38.0 source archive | `34F9EA6FF8636F2C9241153B9114EEFA9E65674A45318AE1EF95BB5F31C53BB2` |
| musl 1.2.6 source archive | `D585FD3B613C66151FC3249E8ED44F77020CB5E6C1E635A616D3F9F82460512A` |
| `userspace/busybox/busybox.config` | `3FBC0403C6A4865FC4397240961C367EE9B36D6D350CC6CEB2D22CBBBEA28480` |
| `userspace/busybox/busybox-uname.config` | `6D972C7A1F3DF0034D5996CC24B58B7364EFBB7851F926C5D8D2FD18C41EBB2B` |
| BusyBox license from the archive | `BBFC9843646D483C334664F651C208B9839626891D8F17604DB2146962F43548` |
| musl copyright record from the archive | `B870108EC5E7790E9F9919064F1B9421D62D5F9B0E6C230C6ADF7EA2DA62E97B` |

The source archives are committed under `userspace/busybox/source/`. CI uses
Ubuntu 24.04, GCC 13.3, binutils 2.42, and a musl 1.2.6 `musl-gcc` wrapper.

## Reproduction

```sh
bash tools/build-busybox-proof.sh build/busybox-contract build/busybox-work
bash tools/build-busybox-uname-proof.sh \
    build/busybox-uname-contract build/busybox-uname-work
```

Each script performs two clean source/toolchain builds and requires
byte-identical results. It rejects changed inputs, configurations, output
hashes, unexpected ELF shape, runtime relocations, dynamic dependencies, W+X,
and exercised MMX/SSE/AVX instructions.

The images are deliberately static non-PIE `ET_EXEC` files at fixed high user
addresses. The build selects musl's `crt1.o`, disables linker relaxation, uses
the large code model, and omits unused constructor bookends. The uname build
adds a build-only scalar target attribute to musl `vfprintf`; the published
source archives remain byte-identical.

## Frozen results

| Profile | Executable | Size | Executable SHA-256 | FAT16 fixture SHA-256 |
| --- | --- | ---: | --- | --- |
| v0.8.0 | `echo SAPOTE` | 33,584 | `B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746` | `41513E5D6F4C33F898F887D4F40F37149A29B1AE13B5E8A600495C18A38C7A6F` |
| v0.9.0 | `uname -s` | 38,368 | `389AD6B13804EB7307BA589C8E8A7C702F91302005A7C5FC6E9E99124FCEAF43` | `48C3465E924D1D2B3C8AB659D2783CAC4AF57DFD83504606AD0DF8F64D7316E3` |

Both executables have five program headers, four load segments, no interpreter,
dynamic section, runtime relocation, PIE, shared object, or RWX segment. Their
exact syscall traces and allowlists are committed beside the configurations.

## Release requirement

A release containing either executable must also provide the exact BusyBox and
musl source archives, configuration, BusyBox license, musl copyright record,
build script, checksum manifest, and profile-specific syscall evidence. A
binary is not published unless those records match.

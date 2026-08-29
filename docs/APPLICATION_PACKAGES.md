<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native application manifests and packages

`tools/sapote-package.py` builds, inspects, and installs Sapote application
packages without network access. JSON is only the host-side build input. The
guest validates a deterministic 1,024-byte binary manifest at the Rust
admission boundary.

## Manifest fields

| JSON field | Version 1 rule |
| --- | --- |
| `name` | Required ASCII display name, at most 31 bytes. |
| `identifier` | Required uppercase-normalized 1–8 character FAT short-name component. |
| `executable` | Required System-volume 8.3 path, at most 15 bytes in the binary field. |
| `abi_version` | Must be `1`. |
| `memory_limit` | Page multiple from 64 KiB through 256 MiB. |
| `max_handles` | 1–128, also bounded by the kernel table. |
| `max_threads` | 1–8. |
| `capabilities` | Names from `console`, `system-read`, `data-read`, `data-write`, `time`, `entropy`, `window`, `input`, `network`, and `threads`. |
| `resource_directory` | Optional immutable System 8.3 directory identifier. |
| `data_namespace` | Required 1–8 character directory on Data; relative application paths are rooted here. |
| `icon` | Optional System 8.3 path. |
| `arguments` | At most eight nonempty ASCII strings of at most 31 bytes each. |
| `resources` | Optional list of `{ "path": "NAME.EXT", "source": "host/path" }` records. Sources are resolved relative to the JSON file. |

The binary manifest starts with `SAPOTEA1`, format version and size, ABI
version, limits, capability bits, fixed-width text records, the executable
SHA-256, and zero-filled reserved space. Nonzero reserved bytes, unterminated
text, nonzero text tails, unknown capabilities, or unused argument records are
named refusals.

## Package container and installation

An `.SPK` file contains a 64-byte `SAPOSPK1` header, the 1,024-byte manifest,
and the exact static executable. Container version 1 ends there. Version 2
adds up to 13 deterministic resource records; each has a fixed 32-byte header,
an 8.3 path, a byte length, zero reserved bytes, and its exact payload. The
package header records all component lengths and the SHA-256 of the complete
body. The manifest contains a second digest of the executable. Inspection
verifies both digests and every resource boundary before returning content.

Typical commands are:

```sh
python3 tools/sapote-package.py build \
    --spec apps/my-app/manifest.json \
    --executable build/my-app/MYAPP.APP \
    --output build/my-app/MYAPP.SPK
python3 tools/sapote-package.py inspect build/my-app/MYAPP.SPK
python3 tools/sapote-package.py install-system \
    --output build/my-app/system.raw build/my-app/MYAPP.SPK
```

Installation rejects symlink inputs, duplicate identifiers, malformed short
names, package-length disagreement, and either digest mismatch. It atomically
replaces its host output through a sibling temporary file. The resulting FAT32
System image contains `IDENT.MAN`, the executable, and any packaged resource
files under `resource_directory`. Resource directories are one cluster and one
level deep by format contract. A process can address its own resources using
relative System paths; the kernel prefixes the admitted directory, so it cannot
escape into another package. The loader creates the named Data directory, and
the native path layer prevents `..`, absolute paths, backslashes, drive syntax,
and cross-namespace access.

# Sapote identity

Sapote is the only current public name of this project. Public prose uses
`Sapote`; paths, command-line keys, symbols, crate and build artifact names use
`sapote`; preprocessor guards and build environment variables use `SAPOTE`.
The interactive prompt is `sap>`.

`Sapote Redwood` names the current graphical shell. `Sapote Redwood`
remains the historical v1/v2.0 release name; neither is a separate product.

## Canonical mark

[`assets/sapote-logo-source.png`](../assets/sapote-logo-source.png) is the
unaltered supplied 643×606 source of truth. Its SHA-256 is:

    90F1C5613AF4EAA817BBF69B151FC2E387BA45873643CC7C220BB471423C6663

Do not redraw, trace, recolour, crop, mirror, add type to it, or substitute a
visually similar mark. Public uses preserve its aspect ratio. The transparent
runtime derivative is [`assets/sapote-logo.png`](../assets/sapote-logo.png),
SHA-256
`F7D932CFB5B2FCC7EC9A33291326217CC17E2E36C604A880083BA7BB459FA912`.
It removes only the source's white matte so the exact red S mark can sit
cleanly on every code-native surface; it does not replace the supplied source.

The kernel does not parse PNG. `tools/make-logo-asset.py` deterministically
fits the source within a 280-pixel ceiling using a bounded premultiplied box
filter. The resulting 280×278 image is encoded as a 188,798-byte `SRL1` stream
and embedded by Rust. This is a runtime presentation size, not a replacement
source asset. The boot proof decodes it into guest memory and compares every
drawn pixel with the decoded stream before reporting success.

## Palette

The logo supplies no interface palette. Sapote Redwood uses a separate palette
around the unchanged asset:

| Role | Value |
| --- | --- |
| Ink and outlines | `#182124` |
| Deep desktop fallback | `#071622` |
| Active title | `#1C292D` |
| Inactive title | `#919DA2` |
| Teal accent | `#68A9C5` |
| Gold accent | `#E6C462` |
| Green accent | `#8EAD89` |
| Red close control | `#D9554F` |
| Violet maximize control and accent | `#947BB4` |
| White/grey minimize control | `#F3F4F5` / `#82888B` |
| Shadow | `#050C12` |
| Window face | `#D9DFE0` |
| Warm white | `#F8FAF8` |

Accent colors belong to tool icons, not readiness indicators or the logo.
Sapote Redwood displays the decoded red S mark in the menu bar, Settings,
Terminal, and other operating-system identity surfaces.
The framebuffer console uses pale green on near-black; early VGA text remains
bright white on black. The kernel has no general alpha compositor or runtime
theme selection.

Sapote Redwood uses one-pixel outlines, platinum bevels, bitmap type, a
compact menu strip, a photographic blue-hour desktop, and a centered reflective
3D Dock. Files uses a compact source list and icon grid; Terminal and Notes keep
the same bounded chrome. Its interaction grammar is informed by late-classic
desktop interfaces; the S mark, composition, labels, and code-native icons are
Sapote's own.

## Voice

Public copy uses short, direct descriptions and avoids broad platform claims.
The welcome screen uses sentence case and human language; proof terms such as
`PASS`, `READY`, and `ONLINE` belong in diagnostic tools, not desktop chrome.

## Public surfaces

The identity applies to the repository name and description, topics, release
titles, boot menu, kernel transcript, panic diagnostics, shell prompt and
version command, documentation, include namespace, C/Rust ABI symbols, crate,
command-line key, internal asset magic, ELF, map, static library and ISO
artifacts. A current surface containing an earlier project name is a rebrand
defect.

New commits, tags, releases, and artifacts use Sapote. Repository history is
changed only by an explicit, reviewed maintenance operation with a recovery
bundle and tree-equivalence proof.

## Verification

`make verify` pins the exact source hash, 280-pixel runtime ceiling, branded ABI
symbols, artifact names, transcript, and prompt. `screen`, `shell` and normal
scenarios exercise the black-and-white console and `sap>` prompt. `redwood-proof`
verifies the installed palette, typed layout, exact decoded logo, and real
framebuffer pixels. QMP captures the committed runtime screenshots; the images
are not manually edited.

# Sapote identity

Sapote is the only current public name of this project. Public prose uses
`Sapote`; paths, command-line keys, symbols, crate and build artifact names use
`sapote`; preprocessor guards and build environment variables use `SAPOTE`.
The interactive prompt is `sap>`.

`Sapote First Light` names the graphical milestone. It is part of Sapote, not a
separate product.

## Canonical mark

[`assets/sapote-logo.png`](../assets/sapote-logo.png) is the source of truth. It
is the exact supplied 375×332 opaque RGBA image. Its SHA-256 is:

    DBDA2F52A5F66CD2F9EFA202CB892C7AB45A29DF83DB37C5C6FDD79B1DEE7CB0

The 18,756-byte file is fully opaque. Its near-white field and charcoal
anti-aliased pixels are part of the asset. Do not redraw, trace, recolour, crop,
mirror, key out its background, add type to it, or substitute a visually
similar mark. Public uses preserve its aspect ratio.

The kernel does not parse PNG. `tools/make-logo-asset.py` deterministically
fits the source within a 280-pixel ceiling using a bounded premultiplied box
filter. The resulting 280×248 image is encoded as a 21,573-byte `SRL1` stream
and embedded by Rust. This is a runtime presentation size, not a replacement
source asset. The boot proof decodes it into guest memory and compares every
drawn pixel with the decoded stream before reporting success.

## Palette

The logo supplies no interface palette. First Light uses a separate palette
around the unchanged asset:

| Role | Value |
| --- | --- |
| Ink and outlines | `#1B1D22` |
| Desktop | `#6E7FA4` |
| Desktop rule | `#8294B8` |
| Active title | `#233A68` |
| Inactive title | `#65728E` |
| Teal status | `#2F8B8C` |
| Gold status | `#D8A43A` |
| Green status | `#4F8A5B` |
| Red status | `#B84E4C` |
| Violet status | `#7B5B89` |
| Shadow | `#5E626B` |
| Window face | `#C8CBD0` |
| White windows and highlights | `#FFFFFF` |

Status accents identify compact indicators and launcher icons; they are not
applied to the logo. The First Light workbench displays the exact decoded shape
as a two-color, two-pixel bitmap with ordered edge dithering directly on the
grey window face, without a separate field or frame. This display-only
treatment leaves the canonical PNG unchanged. The framebuffer console uses
black on white; early VGA
text uses bright white on black. The kernel has no alpha compositor, gradients,
or runtime theme selection.

First Light uses one-pixel outlines, beveled controls, bitmap type, a compact
menu strip, patterned desktop, utility status window, and bottom launcher tray.
The composition and code-native icons are Sapote's own; it does not use or
claim another company's name, wordmark, icons, or trade dress.

## Voice

Public copy uses short technical descriptions and avoids broad platform claims.
First Light's greeting is `hello from the metal.`

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
scenarios exercise the black-and-white console and `sap>` prompt. `first-light`
verifies the installed palette, typed layout, exact decoded logo, and real
framebuffer pixels. QMP captures the committed runtime screenshots; the images
are not manually edited.

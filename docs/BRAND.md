# Sapote identity

Sapote is the only current public name of this project. Public prose uses
`Sapote`; paths, command-line keys, symbols, crate and build artifact names use
`sapote`; preprocessor guards and build environment variables use `SAPOTE`.
The interactive prompt is `sap>`.

`Sapote First Light` names the graphical milestone. It is part of Sapote, not a
separate product.

## Canonical mark

[`assets/sapote-logo.png`](../assets/sapote-logo.png) is the source of truth. It
is the exact supplied 1024×943 transparent RGBA image. Its SHA-256 is:

    15C13E740D26BED1019E99C7FE5CE1B9E293F2A1712BFFFF51EAD3ED2C37A4FE

The 37,400-byte file contains the black pebble and its supplied transparent
field. Do not redraw, trace, recolour, crop, mirror, flatten its transparency,
add type to it, or substitute a visually similar mark. Public uses preserve
its aspect ratio.

The kernel does not parse PNG. `tools/make-logo-asset.py` deterministically
fits the source within a 280-pixel ceiling using a bounded premultiplied box
filter. The resulting 280×258 image is encoded as an 8,693-byte `SRL1` stream
with 1,737 runs and embedded by Rust. This is a runtime presentation size, not
a replacement source asset. The boot proof decodes it into guest memory and
compares every drawn pixel with the decoded stream before reporting success.

## Palette

The logo supplies no interface palette. First Light uses a separate palette
around the unchanged asset:

| Role | Value |
| --- | --- |
| Ink and outlines | `#101012` |
| Desktop | `#595976` |
| Desktop rule | `#666684` |
| Active title and selected tools | `#18181C` |
| Inactive title rule | `#7A7A82` |
| Teal accent | `#4F837F` |
| Gold accent | `#C4A44E` |
| Green accent | `#598561` |
| Red accent | `#A55050` |
| Violet accent | `#705984` |
| Shadow | `#353542` |
| Window face | `#D7D6CE` |
| Warm white | `#F7F6F0` |

Accent colors belong to tool icons, not readiness indicators or the logo.
First Light displays the exact decoded shape once, at its intended presentation
size, as a one-pixel two-color bitmap with ordered edge dithering directly on
the warm window face. The menu bar is type-only: it does not put the pebble in
a miniature tile or frame. This display-only
treatment leaves the canonical PNG unchanged. The framebuffer console uses
black on white; early VGA
text uses bright white on black. The kernel has no alpha compositor, gradients,
or runtime theme selection.

First Light uses one-pixel outlines, platinum bevels, striped title regions,
bitmap type, a compact menu strip, a slate-violet pinstriped desktop, a left
Workspace palette, and a vertical tool dock. Its interaction grammar is
informed by classic Macintosh and NeXT workstation interfaces; the pebble,
composition, labels, and code-native icons are Sapote's own.

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
scenarios exercise the black-and-white console and `sap>` prompt. `first-light`
verifies the installed palette, typed layout, exact decoded logo, and real
framebuffer pixels. QMP captures the committed runtime screenshots; the images
are not manually edited.

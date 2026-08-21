# Sapote identity

Sapote is the only current public name of this project. Public prose uses
`Sapote`; paths, command-line keys, symbols, crate and build artifact names use
`sapote`; preprocessor guards and build environment variables use `SAPOTE`.
The interactive prompt is `sap>`.

`Sapote First Light` names the graphical milestone. It is part of Sapote, not a
separate product.

## Canonical mark

[`assets/sapote-logo.png`](../assets/sapote-logo.png) is the source of truth. It
is the supplied mark with only the three user-identified lower tail decorations
removed, on the original 550×556 opaque RGBA canvas. Its SHA-256 is:

    807CB475A547B371EBB731DB1F07AA8FBE223BFCC235D9554F15B69F4E1CAD1C

The file is fully opaque, including its warm-white field. Do not redraw, trace,
recolour, crop, mirror, key out its background, add type to it, or substitute a
visually similar mark. Public uses preserve its aspect ratio.

The kernel does not parse PNG. `tools/make-logo-asset.py` deterministically
fits the source within a 280-pixel ceiling using a bounded premultiplied box
filter. The resulting 277×280 image is encoded as a 92,763-byte `SRL1` stream
and embedded by Rust. This is a runtime presentation size, not a replacement
source asset. The boot proof decodes it into guest memory and compares every
drawn pixel with the decoded stream before reporting success.

## Palette

First Light takes its rainbow directly from the supplied mark and gives it
classic monochrome personal-computer chrome:

| Role | Value |
| --- | --- |
| Orange | `#E96503` |
| Teal | `#008E92` |
| Yellow | `#FDDA02` |
| Purple | `#782CB2` |
| Red | `#E71F21` |
| Lime | `#A6DF20` |
| Blue | `#018DD8` |
| Platinum desktop | `#DDDAD5` |
| Shadow | `#777777` |
| Black type and outlines | `#000000` |
| White windows and highlights | `#FFFFFF` |

The seven accents follow the mark's left-to-right stripe order and appear in
that order from top to bottom in First Light. The framebuffer
console uses black on white; early VGA text uses bright white on black. There
is no floating-point compositor, translucent theme layer, gradient invented by
the kernel, or runtime theme selection.

The interface deliberately recalls early personal computers: one-pixel
outlines, platinum controls, high-contrast bitmap type, horizontal title-bar
hatching, a menu strip, and the seven-colour band. It does not use or claim
another company's name, wordmark, icons, or trade dress.

## Voice

Sapote is small, direct, warm, and technically exact. Public copy should prefer
plain descriptions such as “a small proof-driven operating system” over grand
platform claims. First Light's greeting is `hello from the metal.` Engineering
documents remain precise and formal where a machine contract demands it.

## Public surfaces

The identity applies to the repository name and description, topics, release
titles, boot menu, kernel transcript, panic diagnostics, shell prompt and
version command, documentation, include namespace, C/Rust ABI symbols, crate,
command-line key, internal asset magic, ELF, map, static library and ISO
artifacts. A current surface containing an earlier project name is a rebrand
defect.

Historical commit objects and already-published tags are immutable records and
are not rewritten or deleted. New commits, tags, releases, and artifacts use
Sapote.

## Verification

`make verify` pins the exact source hash, 280-pixel runtime ceiling, branded ABI
symbols and artifact names, and rejects legacy identity strings or tracked
legacy filenames. Normal boots require the Sapote transcript. `screen`, `shell`
and normal scenarios exercise the black-and-white console and `sap>` prompt.
`first-light` verifies the installed palette, typed layout, exact decoded logo,
and real framebuffer pixels. QMP captures the committed runtime screenshots;
the images are not manually edited.

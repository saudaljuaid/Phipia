# Pyrenis identity

Pyrenis is the only current public name of this project. Public prose uses
`Pyrenis`; paths, command-line keys, symbols, crate and build artifact names use
`pyrenis`; preprocessor guards and build environment variables use `PYRENIS`.
The interactive prompt is `pyr>`.

`Pyrenis First Light` is the public milestone name and `Pyrenis desktop shell`
describes the implemented interface. First Light does not rename the operating
system and is not presented as a separate product.

## Canonical mark

`assets/pyrenis-logo.png` is the source of truth. It is the exact supplied
396×335, non-interlaced, 8-bit RGBA PNG and has SHA-256:

    32CB82EE804EEE0E3F8D3583BDAA4CA88D8E05994F6F58DAA674364883FA92E6

Do not redraw, trace, recolour, crop, mirror, add type to, or replace this file
with a visually similar asset. Resizing for a presentation is allowed only when
the aspect ratio is preserved. The kernel build ceiling is 1024 pixels, so this
source enters the runtime asset at its native 396×335 geometry with no resize.

The kernel does not parse PNG. `tools/make-logo-asset.py` converts the committed
source to the bounded `PRL1` stream embedded by Rust, and the boot proof compares
every drawn pixel with the decoded source before reporting success.

## Palette

The palette is deliberately small:

| Role | Value | Use |
| --- | --- | --- |
| Pyrenis bronze | `#806230` | Mark and graphical-console text |
| White | `#FFFFFF` | Mark field and graphical-console background |
| Deep brown | `#2A2117` | Repository contrast, First Light title bars and active controls |
| Muted bronze | `#A9874E` | Secondary accents and pressed controls |
| Pale bronze | `#E5DFD5` | First Light one-pixel chrome and control strip |

The pale value is the documented bounded tint `(bronze + 4 * white) / 5`,
computed per channel with integer division. No unrelated colour is introduced.

The graphical console uses bronze on white. White is intentional: it joins the
exact white field of the source logo to the surrounding framebuffer without
keying out, editing, or inventing transparency. Early VGA text uses the closest
bounded hardware brown until the framebuffer console exists.

## Public surfaces

The identity applies to the repository name and description, release titles and
tag annotations, boot menu, kernel transcript, panic diagnostics, shell prompt
and version command, documentation, include namespace, C/Rust ABI symbols,
crate, command-line key, ELF, map, static library and ISO artifacts. A current
surface containing an earlier project name is a rebrand defect.

Historical commit objects are immutable records and are not rewritten. A
release or annotated tag is a current presentation surface and therefore uses
Pyrenis even when it points to an older milestone commit.

## Verification

The source asset hash above is checked during review. `make verify` builds the
native-size mark, verifies the branded ABI symbols and artifact names, and the
normal boot contract requires the Pyrenis transcript. The `screen`, `shell` and
normal scenarios exercise the white-and-bronze console and `pyr>` prompt on the
actual framebuffer. `first-light` verifies the same palette in the installed
desktop, and QMP captures the three unedited runtime PNGs under `assets/`.

# First Light UI font

First Light uses Spleen 8x16 version 2.2.0 by Frederic Cambus. The committed
source is `assets/fonts/spleen-8x16.bdf`; its exact upstream release is
<https://github.com/fcambus/spleen/releases/tag/2.2.0>. Spleen is distributed
under the BSD 2-Clause license, committed verbatim as
`assets/fonts/Spleen-LICENSE`.

The BDF SHA-256 is:

    4A3D97EE61A8C86A7525D8C723CB8A14081F395CD2FEB4227BA5E3BAF0629BAE

This is a dedicated licensed face. It is not the existing Sapote console
glyph table, even though both deliberately occupy an 8 by 16 cell.

## Build boundary

`tools/make-ui-font-asset.py` is the only BDF reader. It requires the pinned
8x16 bounding box, ascent 12, descent 4, fixed advance 8, and every printable
ASCII glyph from U+0020 through U+007E. Each row is packed into one byte. A
clone therefore needs no host font library and the running kernel never parses
BDF, PCF, TTF, or OTF.

The build output is Sapote UI Font version 1 (`SUF1`):

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `SUF1` |
| 4 | 1 | format version, currently 1 |
| 5 | 1 | header length, 24 |
| 6..11 | 6 | width, height, ascent, descent, advance, row bytes |
| 12 | 4 | first code point, little-endian |
| 16 | 4 | glyph count, little-endian |
| 20 | 4 | bitmap data length, little-endian |
| 24 | 1520 | 95 consecutive glyphs, 16 rows each |

The complete asset is 1544 bytes. Its SHA-256 is
`D6AD364D9E4A932EB753B83C7EF866DDAF09DDFF8B66BC9669F844267A26CE74`
and its FNV-1a receipt fingerprint is `0xF072CBC7D84A2A20`.

## Runtime validation

Rust remains at the untrusted byte boundary in `src/rust/ui_font.rs`. It checks
the fixed header, version, maximum dimensions, ascent-plus-descent equality,
advance, row width, code-point range, checked glyph/body arithmetic, exact
declared length, and every requested glyph range before copying. The C side
retains only copied metrics after the exact pinned asset is verified.

Named statuses cover null argument, malformed header, unsupported version, bad
metrics, missing glyph, truncated bitmap, size overflow, short output buffer,
destination clipping failure, and use before verification. Drawing is
foreground-only bitmap coverage with integer coordinates. Every glyph is
clipped against both its declared text bounds and the current damage clip.

The Rust pure test constructs one valid synthetic asset and changes one field
at a time to exercise every parser refusal. The C pure test draws `SAPOTE`
into a 64 by 16 synthetic surface and requires stable pixel hash
`0x758397732814F8AF`; it separately proves vertical clipping and a missing
glyph. Corrupting the embedded asset's magic or removing its final byte is
reported respectively as `UI font header is missing or malformed` and
`UI font bitmap is truncated` before any desktop draw.

The Boot Ledger font receipt records exact packed size and FNV fingerprint.
Installed verification re-reads those values and requires width 8, height 16,
ascent 12, descent 4, advance 8, first U+0020, and count 95.

## Limits

Only printable ASCII is installed. There is one weight and one size, no
kerning, Unicode shaping, dynamic loading, scaling, antialiasing, fallback, or
runtime font parser. Extending the glyph range changes the packed asset and its
receipt and therefore requires an explicit format/proof review.

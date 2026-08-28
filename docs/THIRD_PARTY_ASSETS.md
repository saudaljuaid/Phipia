<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Third-party visual assets

Sapote's build is offline and deterministic. The exact third-party source
files used by the UI are committed, licensed beside the assets, converted by
host tools, and then parsed through bounded Rust formats before C draws them.

## Inter

The graphical shell uses `InterVariable.ttf` from the official Inter repository
at commit `353b61b9f4430d5f420d56605a6e7993e0941470`. Inter is distributed under
the SIL Open Font License 1.1; the exact license is committed as
`assets/fonts/Inter-LICENSE.txt`.

`tools/rasterize-inter-ui.py` converts printable ASCII at the pinned size into
the committed alpha atlas and proportional metrics. The ordinary build does
not need Pillow and does not parse TrueType: `tools/make-ui-font-asset.py`
packs those committed intermediates into SUF2, whose exact metrics, length,
fingerprint, glyph ranges, and alpha data are validated before installation.

## Lucide

The twelve Settings category pictograms come from the official Lucide
repository at commit `23f9abc4ed0146cffededd3d7f94c1018bfdf693`. The selected
SVG sources and Lucide ISC license are committed under `assets/icons/lucide/`.

`tools/rasterize-settings-icons.py` composes those pictograms into one coherent
4×3 glossy category sheet. The sheet is committed, so CairoSVG is a development
regeneration dependency only; a normal Sapote build remains self-contained.

## 3d-dock

Sapote's Dock interaction and glass-shelf geometry are a native fixed-point
port of [`saudaljuaid/3d-dock`](https://github.com/saudaljuaid/3d-dock) at
commit `8ab14d0c372ab797475e49b8a658d54f30f706bc`. The original implementation is
written in C with Cairo and X11. Sapote preserves its raised-cosine hover
curve, pointer-anchored layout, eased panel width, press squash, decaying
bounce, trapezoidal shelf flare, warped reflection strips, running lights, and
tooltip fades, while replacing those hosted dependencies with bounded Q16.16
math and direct cached-framebuffer drawing. Only Sapote's six applications are
present. The upstream MIT license is committed as
`docs/third-party/3d-dock-LICENSE`.

## Sapote photographic scenes

The fourteen wallpaper sources are real photographs downloaded from Unsplash
and used under the [Unsplash License](https://unsplash.com/license). The exact
cropped 1024x768 PNGs are committed; the original source pages are:

- [Purple galaxy](https://unsplash.com/photos/a-purple-and-blue-space-filled-with-stars-NYwZhS4afQc)
- [Star field](https://unsplash.com/photos/a-galaxy-surrounded-by-stars-in-space-Rw7H7ELqJS4)
- [Milky Way lake](https://unsplash.com/photos/milky-way-over-mountain-lake-A2N0wk8k70g)
- [Milky Way reflection](https://unsplash.com/photos/milky-way-and-mountains-reflected-in-a-serene-lake-HUYPJupBvwE)
- [Forest waterfall](https://unsplash.com/photos/a-waterfall-in-a-forest-DIFY95lLyzU)
- [Waterfall valley](https://unsplash.com/photos/scenery-of-waterfalls-9NgKOxVY4wM)
- [Desert dunes](https://unsplash.com/photos/a-group-of-sand-dunes-in-the-desert-HEf0fKgJA1Q)
- [Aurora](https://unsplash.com/photos/aurora-borealis-jwIk4Z3Msi4)
- [Aurora fjord](https://unsplash.com/photos/aurora-borealis-above-mountain-and-body-of-water-l9cneQNE03Y)
- [Golden mist forest](https://unsplash.com/photos/misty-forest-landscape-with-soft-golden-light-tkoA1sWwHUg)
- [Yosemite mist](https://unsplash.com/photos/forest-covered-by-mist-xPmdw_RliUA)
- [Alpine lake](https://unsplash.com/photos/alpine-lake-reflecting-mountains-under-clear-blue-sky-4vGDNyXH8n0)
- [Tropical sunset](https://unsplash.com/photos/tropical-coastline-with-palm-trees-at-sunset-o_X5J_5wsYQ)
- [Ocean cliffs](https://unsplash.com/photos/rocky-cliffs-overlook-calm-ocean-at-sunset-DGDl6JQpgBw)

They are packed without another resize into the deterministic RGB565 SPW3
album used by Desktop wallpaper selection. The 16-bit source path avoids the
visible palette dithering of the earlier RGB332 prototype while keeping
PNG/DEFLATE outside the kernel trust boundary.

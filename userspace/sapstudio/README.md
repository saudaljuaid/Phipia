<!-- SPDX-License-Identifier: GPL-3.0-only -->

<h1 align="center">SapStudio</h1>

<p align="center">
  <strong>A non-linear video editor designed for Sapote.</strong>
</p>

<p align="center">
  <a href="LICENSE"><img
    src="https://img.shields.io/badge/license-GPL--3.0--only-595976"
    alt="GPL-3.0-only"></a>
  <img src="https://img.shields.io/badge/status-foundation-18181C"
    alt="foundation stage">
</p>

## Status

SapStudio's portable editor core is implemented and tested. Redwood currently
provides a native workspace with BMP import, timeline editing, project save,
and BMP export. The full editor still needs a general Sapote application ABI,
larger userspace memory mappings, framebuffer and input services, audio
streaming, and broader media I/O.

## Available today

### Editing model

- Rational time, timebases, drop-frame timecode, and half-open ranges.
- Sequences, tracks, clips, gaps, transitions, masks, titles, markers, and
  exact-rational retiming.
- Insert, overwrite, trim, roll, slide, razor, merge, lift, and drop edits.
- Undo and redo with a versioned project format and recovery-safe save path.
- Keyframed opacity, transforms, masks, grades, rotation, and fades.

### Media and rendering

- Typed frame and colour descriptions, deterministic frame pooling, and the
  uncompressed `SPRW` mezzanine format.
- Render graphs, compositing in linear light, gamut conversion, transfer
  functions, LUTs, resampling, rasterized shapes, titles, and scopes.
- Audio buffers, gain, panning, mix buses, waveform summaries, and BS.1770
  loudness measurement.
- PNG reference output and CMX 3600 interchange.

### Runtime work

- A freestanding static ELF64 image at Sapote's user address.
- A small C ABI layer and allocator/runtime support.
- No dynamic section, relocations, SIMD instructions, or hosted runtime
  dependency in the target image.

1066 tests, no third-party dependencies in the current implementation. Unsafe
Rust is restricted to the ABI and runtime crates.

## Repository layout

| Path | Purpose |
| --- | --- |
| `crates/` | Rust workspace |
| `native/` | Small C ABI shims |
| `perf/` | Optional measured C++ leaves |
| `targets/` | Freestanding target and linker script |
| `tools/` | Build and validation utilities |
| `fuzz/` | Parser fuzz targets and corpora |
| `tests/golden/` | Reference output |
| `docs/` | Architecture and policies |

Rust owns the project model, media pipeline, UI state, and coordination. C is
limited to narrow platform boundaries. C++ is reserved for measured inner
loops that retain a bit-exact Rust reference.

## Documents

- [Charter](docs/CHARTER.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Platform contract](docs/PLATFORM_CONTRACT.md)
- [Roadmap](docs/ROADMAP.md)
- [Engineering rules](docs/ENGINEERING_RULES.md)
- [Dependency policy](docs/DEPENDENCY_POLICY.md)
- [Open-source map](docs/DEPENDENCIES.md)
- [Verification](docs/VERIFICATION.md)
- [Brand](docs/BRAND.md)
- [Glossary](docs/GLOSSARY.md)

## Build

```sh
make hooks
make lint
make check
make test
make image
make audit
make reproducible
make verify
```

`make verify` is the normal local gate. See
[Verification](docs/VERIFICATION.md) for the role of each check.

## Licence

[GPL-3.0-only](LICENSE).

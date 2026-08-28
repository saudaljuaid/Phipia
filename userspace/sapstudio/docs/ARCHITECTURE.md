<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Architecture

SapStudio separates the editor model from platform services and format code.
The host build exercises the same model and renderer used by the freestanding
image.

## Crates

| Crate | Owns | `unsafe` | State |
| --- | --- | --- | --- |
| `sapstudio-core` | Rational time, timecode, identifiers, fixed-point arithmetic, digests | forbidden | **exists**, 79 tests |
| `sapstudio-model` | Projects, sequences, tracks, clips, edits, history, transitions, masks, titles, markers, automation | forbidden | **exists**, 342 tests |
| `sapstudio-abi` | Platform traits and all external raw-pointer boundaries | **permitted** | **exists** |
| `sapstudio-rt` | Program entry, allocator, panic path, and mapping support | **permitted** | **exists** |
| `sapstudio-media` | Frame and sample types, colour descriptions, content identity, frame pool, test patterns | forbidden | **exists**, 42 tests |
| `sapstudio-io` | Project files, reels, save protocol, EDL, LUT, PNG, and bounded readers and writers | forbidden | **exists**, 213 tests |
| `sapstudio-app` | Commands, playback policy, session lifetime, timeline rendering, and mixdown | forbidden | **exists**, 74 tests |
| `sapstudio-image` | Freestanding entry point | **permitted** | **exists**, audited |
| `sapstudio-render` | Render graph, compositor, colour pipeline, LUTs, rasterization, titles, and scopes | forbidden | **exists**, 239 tests |
| `sapstudio-audio` | Mixer, gain, panning, loudness, and waveform summaries | forbidden | **exists**, 77 tests |
| `sapstudio-ui` | Widgets, layout, damage tracking, and interface state | forbidden | planned |

Only `sapstudio-abi`, `sapstudio-rt`, and the image entry may contain unsafe
Rust. The remaining crates use `#![forbid(unsafe_code)]`.

## Dependency direction

```text layers
0  sapstudio-core
1  sapstudio-abi
2  sapstudio-media  sapstudio-model  sapstudio-audio
3  sapstudio-rt  sapstudio-render
4  sapstudio-io
5  sapstudio-app
6  sapstudio-image
```

Dependencies point from a higher numbered layer to a lower numbered one. Model
code does not import render, UI, or platform types. Format code converts bytes
into model or media values but does not own editing behavior. The app crate
coordinates the layers.

## Platform seams

| Seam | Purpose |
| --- | --- |
| `Console` | Diagnostic output |
| `Time` | Monotonic timestamps and deadlines |
| `Presentation` | Pixel surface acquisition and damage submission |
| `Input` | Keyboard and pointer events |
| `Storage` | Bounded reads, writes, synchronization, and replacement |
| `Audio` | Sample submission and playback position |

Host tests provide deterministic implementations. Native implementations stay
inside `sapstudio-abi`.

## Project model

```text
Project
  media library
  sequences
  edit history

Sequence
  timebase
  ordered tracks
  markers

Track
  non-overlapping clips and gaps
  transitions
  opacity and level automation

Clip
  media identity and source range
  duration and exact-rational speed
  transform, mask, grade, fade, and keyframes
```

Track items cannot overlap. Transitions describe a cut between neighboring
items instead of occupying time as another item. Media is identified by content
digest; a filesystem location is only a hint.

Edits produce an inverse operation for undo. An edit either applies completely
or leaves the project unchanged. Saved projects use stable identifiers and a
versioned encoding.

## Saving

The save path is:

1. encode the project;
2. write a temporary file;
3. read and validate it;
4. synchronize storage;
5. replace the previous project.

Failure before replacement keeps the previous file. The decoder checks version,
lengths, capacities, identifiers, checksums, and trailing data before returning
a model.

## Media pipeline

```text
source → decode → describe → cache → graph → composite → present or export
```

Frames always carry dimensions, sample format, colour primaries, transfer
function, matrix, range, chroma placement, and coverage interpretation when
present. Cache keys include the source digest, operation parameters, and code
version.

The render graph contains pure operations and typed inputs. Nodes may only
reference earlier nodes, preventing cycles. Evaluation order must not change
the result.

The audio path follows the same structure: decode, resample, mix, meter, and
submit. Real-time submission cannot allocate or lock.

## Concurrency

The current target is single-core and evaluates graphs serially. Graph nodes do
not depend on ambient mutable state, and reduction order is fixed so the same
graph can move to parallel execution later without changing output.

## Directory layout

```text
crates/            Rust workspace
native/            C ABI shims
perf/              optional optimized leaves
targets/           target specification and linker script
tools/             build and validation tools
fuzz/              fuzz targets and corpora
tests/golden/      reference output
docs/              architecture and policy
assets/            application assets
```

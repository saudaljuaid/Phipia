<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Roadmap

This roadmap describes the remaining product work. Completed implementation
details belong in the architecture and changelog, not in a running diary.

## Current baseline

The editor core already includes rational time, a project and edit model,
undo/redo, versioned persistence, frame and audio types, a render graph,
compositing, colour conversion, LUTs, scopes, transitions, masks, titles,
retiming, keyframes, interchange, and a freestanding Sapote image.

Redwood exposes a smaller integrated surface: BMP import, clip selection and
trim, project save, and BMP export.

## Near-term priorities

### Native application services

- Define a versioned Sapote application ABI.
- Load an application image without pinning it as a special kernel fixture.
- Add growable userspace mappings and explicit process lifetime services.
- Expose framebuffer surfaces, input events, time, and writable paths to an
  application.

This work is complete when SapStudio launches as a userspace program and can
open, edit, save, close, and reopen a project without kernel-owned UI code.

### Interactive editor

- Connect the project model to source, viewer, timeline, inspector, and mixer
  panels.
- Add deterministic layout, keyboard focus, pointer capture, scrolling, and
  damage tracking.
- Route edits through the existing undo journal.

This work is complete when the visible timeline and the saved model remain in
sync through editing and recovery.

### Media pipeline

- Add image-sequence and uncompressed video readers first.
- Connect decode, cache, graph evaluation, compositing, and export.
- Add compressed codecs only through reviewed, vendored dependencies.
- Preserve explicit colour metadata from input through output.

This work is complete when a project can render a reproducible image sequence
and reopen it with matching frame descriptions.

### Audio

- Connect decoded samples to the mixer and meters.
- Add Sapote audio submission, clocking, and underrun reporting.
- Keep allocation and locks outside the real-time path.

This work is complete when synchronized picture and sound play through Sapote
and an offline export matches the same timeline.

### Performance and hardware

- Add saved floating-point and SIMD state to Sapote userspace.
- Measure hot loops before introducing optimized leaves.
- Add userspace threads and multicore graph execution while keeping output
  independent of scheduling order.
- Introduce GPU acceleration only after the software path is complete.

## Release criteria

The first full SapStudio release needs:

- native launch and clean shutdown;
- project creation, import, editing, save, recovery, and export;
- synchronized playback with clear underrun behavior;
- reproducible target images and pinned dependency sources;
- parser fuzzing and reference renders for supported formats;
- documented resource limits and unsupported formats.

## Deferred

Live collaboration, cloud services, plugin hosting, scripting, broad codec
coverage, GPU effects, and distributed rendering are outside the first
release. Their absence should not complicate the core editor.

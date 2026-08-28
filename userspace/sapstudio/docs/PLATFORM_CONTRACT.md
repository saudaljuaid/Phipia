<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Platform contract

SapStudio is designed for Sapote. This document lists the operating-system
services needed to move the existing editor core into a native userspace
application.

## Sapote today

Current Sapote provides:

- Ring 3 execution with checked static ELF64 loading and private address
  spaces;
- several live user processes with saved general-purpose register state;
- kernel-owned framebuffer, keyboard, pointer, and Redwood windows;
- monotonic time and kernel threads;
- read-only system and writable data FAT32 volumes over NVMe;
- a measured Linux compatibility surface and an experimental native networking
  interface;
- an HD Audio codec command transport without sample streaming.

Redwood currently integrates a small SapStudio workspace directly in the
kernel shell. It can import an uncompressed BMP, edit clip duration, save a
project, and export a BMP. This is useful product integration, but it is not the
general application interface required by the full editor.

## Required services

### Application loading

Sapote needs a versioned native ABI and a loader for ordinary signed or
approved application images. Launch, exit, faults, and resource cleanup must
have stable observable behavior.

### Memory

An editor needs growable anonymous mappings and enough virtual address space
for frames, audio, caches, project data, and decoder state. Allocation failure
must be returned to the process rather than converted into a kernel failure.

### Floating point and SIMD

Sapote must enable and preserve x87/SSE state for userspace before SapStudio
can use ordinary media arithmetic or optimized codec libraries. Context
switches, interrupts, faults, and syscalls must preserve the selected state.

### Display and input

Applications need owned framebuffer surfaces or shared presentation buffers,
damage submission, keyboard events, pointer events, focus, capture, and window
lifecycle notifications.

### Storage

SapStudio needs versioned path-based open, read, write, seek, sync, rename,
replace, enumerate, and metadata operations over the writable data volume.
Temporary-file replacement must support project recovery.

### Time and scheduling

Userspace needs monotonic timestamps, deadlines, sleep or wait operations, and
readiness notification. Playback later needs threads and more than one core,
but correctness does not depend on parallelism.

### Audio

The OS must expose negotiated output formats, a submission queue, a playback
position clock, underrun reporting, and clean teardown. The real-time path
cannot depend on filesystem or heap work.

### Entropy and faults

Applications need random bytes for identifiers and clear process-visible fault
reports. A fault in SapStudio must not damage the kernel or another process.

## Later services

GPU acceleration and an IOMMU are valuable but not prerequisites for the first
software-rendered editor. Networking, cloud storage, collaboration, and plugin
hosting are outside the initial native application contract.

## Integration order

1. Native ABI and ordinary application loading.
2. Growable mappings, path-based storage, time, display, and input.
3. A standalone SapStudio process using the existing model and renderer.
4. Audio output and synchronized playback.
5. Saved SIMD state, userspace threads, and multicore rendering.
6. Optional GPU acceleration.

Each service should arrive with a small public ABI, explicit resource limits,
cleanup behavior, and a QEMU test that exercises it from userspace.

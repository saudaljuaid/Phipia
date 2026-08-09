# Day one: prove the execution boundary

## Decision

Zenith starts on x86_64 through the GNU Multiboot2 protocol. GRUB is a temporary
loader, not part of the kernel architecture. It keeps day one focused on the
first code Zenith owns rather than on disk formats and firmware edge cases.

## Boot invariants

1. `_start` receives the Multiboot2 magic in EAX and the information address in
   EBX while running in 32-bit protected mode.
2. Interrupts and the direction flag are cleared before Zenith touches its own
   state.
3. The processor must expose CPUID, MSRs, PAT, long mode, and NX.
4. Six bootstrap page-table pages temporarily identity-map the first 4 GiB with
   2 MiB writable pages.
5. Four-level CR3, CR4.PAE, EFER.LME/NXE, and CR0.PG/WP are established in that
   order; CR4.LA57 is cleared explicitly.
6. A private GDT and 16 KiB, 16-byte-aligned stack exist before C executes.
7. The C entry obeys the x86_64 System V calling convention and uses no red zone,
   host library, stack protector, floating point, MMX, or SSE.
8. Successful execution is observable on COM1 and VGA; failure halts instead of
   continuing in an unknown state.

The identity map is intentionally broad but temporary. After firmware discovery
and frame ownership are complete, Zenith replaces it with explicit 4 KiB kernel,
VGA, and APIC mappings that enforce W^X and device cache policy. A higher-half
layout and process address spaces remain future virtual-memory work.

## Acceptance criteria

- The kernel is ELF64 for x86-64 with a valid Multiboot2 header in the first
  32 KiB.
- Linking has no unresolved symbol and no load segment that is writable and
  executable.
- A clean QEMU boot emits `Zenith OS: day one passed` through COM1 within ten
  seconds.
- The same checks run on every push to `main` and every pull request targeting it.

## Ordered next work

1. ~~Replace broad identity mapping with explicit kernel and device mappings.~~
2. ~~Define exception-entry assembly, an IDT, and fatal exception diagnostics.~~
3. ~~Add a monotonic timer and interrupt-controller abstraction.~~
4. ~~Validate the ACPI root, probe the discovered xAPIC hardware, permanently
   mask the legacy PIC, and route the temporary PIT clock through the I/O APIC
   and Local APIC.~~
5. Calibrate a Local APIC or architectural timer against a trusted source and
   expose a monotonic kernel timebase.
6. Only then introduce heap allocation, scheduling, userspace, storage, or graphics.

Each step must arrive with a narrow invariant and a QEMU-observable test. The UI
is explicitly out of scope until the kernel can isolate processes, survive
faults, and access storage through tested drivers.

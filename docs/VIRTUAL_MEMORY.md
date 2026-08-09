# Permanent virtual-memory foundation

This milestone replaces Zenith's disposable first-4-GiB huge-page map with one
bounded four-level hierarchy of explicit 4 KiB mappings. The bootstrap map still
exists long enough to validate Multiboot2, walk ACPI, select the test scenario,
and establish physical-frame ownership. No firmware-controlled pointer is used
after the permanent CR3 is installed.

## CPU contract

The 32-bit entry path requires CPUID, MSRs, PAT, long mode, and NX. It clears
CR4.LA57 so the hierarchy is unambiguously four levels, enables EFER.NXE, and
sets CR0.WP before entering C. Supervisor writes therefore obey read-only leaf
permissions, and the NX bit is architecturally meaningful.

Before selecting device memory, Zenith serializes caches, writes the
architectural reset layout to IA32_PAT, reads it back, and flushes the TLB. PAT
entry 3 is consequently UC. Device leaves select entry 3 with PWT=1, PCD=1, and
PAT=0. A failed readback aborts before CR3 changes.

## Mapping invariant

The linked image exports page-aligned text, rodata, data, BSS, and kernel bounds.
The permanent builder rejects noncanonical or unaligned addresses, arithmetic
overflow, physical addresses wider than CPUID reports, W+X permissions, invalid
device permissions, remapping, huge-page parents, oversized ranges, and table
arena exhaustion.

The live hierarchy contains only these mappings:

| Virtual range | Physical range | Permissions and cache type |
| --- | --- | --- |
| Multiboot header page | identity | read-only, NX, WB |
| kernel text | identity | read-only, executable, WB |
| kernel rodata | identity | read-only, NX, WB |
| kernel data, BSS, stacks, and page tables | identity | writable, NX, WB |
| `0xB8000` VGA page | identity | writable, NX, UC |
| `0xFFFF800000000000` | discovered Local APIC page | writable, NX, UC |
| following device-window pages | discovered I/O APIC pages | writable, NX, UC |

Page zero, the remainder of low memory, boot information, ACPI tables, free
physical frames, and the 4 GiB fault probe are absent after the transition.
Every hierarchy entry is supervisor-only. The builder uses a 64-page,
page-aligned static arena inside the linked kernel, so the frame allocator
permanently owns and reserves every active table. A linker policy caps the
kernel image at 64 MiB, matching the builder's bounded range.

The builder validates every expected leaf before the transition. It then
normalizes PAT, loads the new CR3 once, and validates CR3, CR0.WP, EFER.NXE, PAT,
every kernel page, VGA, the null and 4 GiB holes, and every APIC device mapping.
If post-load validation fails while the old hierarchy is still usable, Zenith
restores the bootstrap CR3 and previous PAT value before reporting failure.

## Executable proof

An in-kernel rejection suite constructs private hierarchies and proves valid
translation plus rejection of null arenas, zero capacity, alignment errors,
noncanonical virtual addresses, over-width physical addresses, W+X and bad
device permissions, duplicate mappings, range overflow, excessive ranges,
table exhaustion, huge-page conflicts, null or malformed device addresses, and
excess device counts.

Every prior QEMU scenario runs after the permanent transition. Two additional
assembly-only scenarios prove the hardware permission boundary without invoking
undefined C behavior:

1. `write-protect` writes to a rodata byte and must receive a present supervisor
   write page fault (`P=1 W=1 U=0 I=0`).
2. `nx` jumps to a data byte and must receive a present supervisor instruction
   page fault (`P=1 W=0 U=0 I=1`).

The normal scenario must emit:

```text
Zenith OS: virtual memory foundation passed
```

## Deferred work

This is a permanent boot-time map, not a general virtual-memory manager. It has
no dynamic table allocation, unmapping, temporary physical-map window, guard
pages, higher-half kernel relocation, process address spaces, copy-on-write,
TLB shootdown, or SMP synchronization. The APIC-routing and Local APIC timer
milestones consume the device mappings; their ownership and cache policy remain
part of this virtual-memory contract.

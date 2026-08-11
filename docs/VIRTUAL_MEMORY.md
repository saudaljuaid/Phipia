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
| `0xFFFF900000000000` through `0xFFFF900000FFFFFF` | heap-owned frames on demand | writable, NX, WB |
| `0xFFFFA00000000000` through `0xFFFFA0000011FFFF` | task-owned frames in fixed guarded slots | writable, NX, WB |

The heap payload has one absent guard page on each side. Its hierarchy parents
and eight leaf tables are prepared during boot, but all 4096 payload leaves
begin absent. Runtime heap growth may map and roll back individual leaves with
exact physical-frame provenance and `invlpg`; it cannot allocate another table
or select arbitrary permissions. The permanent arena remains 64 pages (256 KiB
of BSS), and the heap hierarchy consumes ten of those pages in this layout.

The task-stack window contains sixteen deterministic 72 KiB slots. Each slot
has one absent lower guard, sixteen payload leaves, and one absent upper guard.
The window consumes exactly three more arena pages: one PDPT, one page directory,
and one leaf table. All parents are prepared before CR3 publication; all 256
payload leaves begin absent. The task-specific runtime surface accepts only a
slot and payload-page index, fixes the leaf policy to supervisor RW+NX WB and
non-global, requires exact task-frame provenance on unmap, and uses `invlpg`.
The 64-page arena is unchanged.

Every actual task-stack leaf write advances a nonzero 64-bit mutation epoch.
Mapping preflights three stamps (the new leaf plus enough lifetime headroom for
a later unmap and possible restoration); unmapping preflights two. Injected
uncertain commits are stamped, and a validation rollback stamps both the first
write and the restoring write. Exhaustion poisons the VM before a leaf can be
changed. An O(1), irqsave-locked certificate returns this epoch with the exact
mapped-page count and clears its output on every failure. A complete audit can
therefore mint a certificate that later scheduler-local validation compares
without rescanning all 256 task-stack leaves.

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

## Interrupt-state and lock contract

Runtime query, map, unmap, validation, and statistics transactions use a
virtual-memory-class irqsave lock. Nested heap growth follows the one permitted
order: `HEAP -> VIRTUAL_MEMORY -> PHYSICAL_MEMORY`. Every operation preserves
entry IF, while mutation entry points additionally require the caller to enter
with IF clear. These APIs are not NMI, IST, or panic safe and those paths must
not call them. This is BSP serialization against maskable-interrupt reentry,
not cross-CPU TLB-shootdown or SMP safety.

Full active validation traverses the 4,096-leaf heap window and 256-leaf
task-stack window exactly once each. Heap validation supplies its committed and
staged backing-frame spans to a combined VM operation, which proves every
present and required-absent heap leaf and returns statistics from the same lock
snapshot. This preserves the full guard, permission, count, hardware-control,
kernel, device, and task-stack checks without duplicate window scans or one VM
lock acquisition per heap page.

## Executable proof

An in-kernel rejection suite constructs private hierarchies and proves valid
translation plus rejection of null arenas, zero capacity, alignment errors,
noncanonical virtual addresses, over-width physical addresses, W+X and bad
device permissions, duplicate mappings, range overflow, excessive ranges,
table exhaustion, huge-page conflicts, null or malformed device addresses, and
excess device counts. Heap-specific rejection also proves the exact table
limit, absent guards, mapping conflict, missing mapping, physical-width checks,
and transactional runtime map/unmap behavior.
Task-stack rejection proves its root-plus-three table boundary, first and last
slot arithmetic, page and slot limits, mapping conflicts, physical-width and
permission checks, absent guards, exact-frame unmap, injected uncertain writes,
and exact mapping-count restoration.

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

This remains a permanent hierarchy with narrow heap and task-stack runtime leaf
extensions, not a general virtual-memory manager. It has no dynamic table
allocation, general unmapping, temporary physical-map window, higher-half
kernel relocation, process address spaces, copy-on-write, cross-CPU TLB
shootdown, or SMP synchronization. The APIC-routing and Local APIC timer
milestones consume the device mappings; their ownership and cache policy remain
part of this virtual-memory contract. Heap ownership and rollback are specified
in `docs/KERNEL_HEAP.md`; guarded task-stack ownership is specified in
`docs/KERNEL_SCHEDULER.md`.

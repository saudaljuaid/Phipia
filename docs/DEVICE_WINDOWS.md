# Physical device windows

Paging used to learn about hardware through its argument list. The local and
I/O APICs arrived in `acpi_topology`, PCI ECAM arrived in `acpi_mcfg`, and the
linear framebuffer arrived in `boot_framebuffer`. Every new physical aperture
therefore changed `paging_initialize` and repeated the same coupling in
`kernel_test_run`. The arguments described discovery mechanisms, not the
address-space policy paging actually needed.

OpenSeneri now builds one `struct paging_device_windows` after boot has discovered
the machine. `paging_initialize` receives only that collection. The scenario
runner receives a `struct kernel_test_context` containing the same collection
alongside the descriptions needed by PCI and framebuffer-specific tests. No
firmware table is rediscovered by paging and no hidden global supplies a test
environment.

## Representation and ownership

`include/seneri/paging.h` defines three public pieces:

- `enum paging_device_window_kind` names the physical device;
- `struct paging_device_window` carries kind, instance, physical base, byte
  length, semantic memory type, and semantic access permissions;
- `struct paging_device_windows` carries a count and a fixed array.

There are no page-table bits in the representation. Memory is requested as
`PAGING_MEMORY_WRITE_BACK`, `PAGING_MEMORY_WRITE_COMBINING`, or
`PAGING_MEMORY_UNCACHEABLE`. Read access is implicit and the only additional
device permission is write. Execute and user access cannot be represented.

The Boot Ledger's device-window stage in `boot_plan.c` owns construction. It
adds VGA, the discovered local APIC, every
bounded I/O APIC, and usable optional ECAM and framebuffer spans. Validation
sorts a private bounded copy into canonical physical-address order, and paging
copies that validated value when it installs the hierarchy. Consumers may read
the installed immutable copy through `paging_get_device_windows`; there is no
live add, remove, or retype operation.

## Capacity and bounds

The capacity is twelve entries:

| Source | Maximum |
| --- | ---: |
| VGA text memory | 1 |
| local APIC | 1 |
| I/O APICs | `ACPI_MAX_IO_APICS` = 8 |
| PCI ECAM | 1 |
| framebuffer | 1 |

The expression is `4 + ACPI_MAX_IO_APICS` and is statically asserted against
the discovery bound. Each range must be non-empty, 4 KiB aligned, addition-safe,
no larger than 16 MiB, and wholly below the 4 GiB early identity limit. Fixed
register windows are exactly one page and ECAM is exactly 2 MiB. Every loop is
bounded by the validated count, the twelve-entry capacity, a validated window
length, or the fixed 4 GiB identity-map page count.

## Kinds and installed policy

| Kind | Instance | Length | Access | Memory type |
| --- | --- | ---: | --- | --- |
| `VGA_TEXT` | 0 | 4 KiB | writable, supervisor, NX | UC |
| `LOCAL_APIC` | 0 | 4 KiB | writable, supervisor, NX | UC |
| `IO_APIC` | discovery index 0-7 | 4 KiB | writable, supervisor, NX | UC |
| `PCI_ECAM` | 0 | 2 MiB | writable, supervisor, NX | UC |
| `FRAMEBUFFER` | 0 | rounded byte span | writable, supervisor, NX | WC |

Ordinary RAM, page-table frames, heap allocations, and the cached drawing
surface remain WB. VGA is fixed at physical `0xB8000`; APIC addresses come from
the already validated MADT topology; ECAM comes from the already validated
MCFG; and framebuffer bytes come from the validated Multiboot2 tag.

The structural validator understands all three semantic memory types so its
pure test can prove mixed WB/WC/UC descriptions and overlap conflicts without
hardware. Boot construction supplies the policy in the table. The installed
proof independently requires UC for every register kind, and the independent
whole-framebuffer proof requires WC for the loader-described display span.

## Alignment, rounding, and optional devices

Registry entries themselves are exact 4 KiB ranges: validation never silently
rounds a caller's values. VGA and APIC addresses are already page aligned.
ECAM retains the earlier compatibility rule that its base must be 2 MiB aligned
and the first 2 MiB must fit below 4 GiB; an absent or unusable MCFG leaves PCI
on configuration mechanism 1.

Framebuffer construction first checks `address + size` without overflow. It
retains the existing eight-2-MiB-region availability bound, then rounds the
start down and end up to 4 KiB so every page intersecting a pixel byte is WC.
An absent framebuffer is valid. A present description that cannot satisfy the
existing bound or overlaps an accepted device window is reported unavailable,
is not partly registered, and has its availability marker cleared so the
framebuffer layer takes the existing serial-only `ABSENT` path.

Absence of both optional kinds is accepted. VGA, the local APIC, and at least
one I/O APIC are required for this boot architecture.

Each optional candidate is checked against the required entries and any
earlier accepted optional entry before it is added. An overlap is reported with
the same named conflict/overlap status and leaves that optional device
unavailable; it does not turn the final required-registry validation into a boot
failure. ECAM then falls back to configuration ports, while a refused
framebuffer leaves the serial console active.

## Duplicates and overlaps

Insertion order is not policy. Validation first checks every bounded entry,
then considers every bounded pair with deterministic refusal precedence:

1. any physical overlap with different memory types is
   `CONFLICTING_DEVICE_WINDOW_OVERLAP`;
2. an exactly identical entry or repeated kind/instance is
   `DUPLICATE_DEVICE_WINDOW`;
3. every other same-memory physical overlap is
   `OVERLAPPING_DEVICE_WINDOWS`.

No entries are merged. A valid collection is canonically sorted by physical
base, length, kind, instance, memory type, and permissions. Reversing insertion
order therefore produces the same counted entries and the same mappings.
Device overlap with the linked kernel is refused separately.

## Named refusals

The registry adds a status for every rejection rather than dropping an entry:

- bad kind or bad instance;
- unsupported memory type or invalid device permissions;
- zero length or unaligned base/length;
- range overflow or a range outside the supported identity window/bound;
- capacity exhausted;
- conflicting-memory overlap, other overlap, or duplicate identity;
- a missing required window;
- overlap with the linked kernel;
- mismatch between the installed hierarchy and the validated registry.

The public status string table and kind-name table are each statically sized
against their count enumerator.

## Page sizes and PAT

The identity builder examines all 2,048 fixed 2 MiB regions below 4 GiB. A
region intersecting the linked kernel or any registered window is split into
4 KiB leaves. Every other region remains one writable, NX, WB 2 MiB leaf. A
single framebuffer entry may cross several 2 MiB boundaries; every intersected
region is split and every page in the registered span receives WC.

The registry changes neither PAT ownership nor transition order. Entry 0 stays
WB, entry 3 stays UC, and only unused entry 1 is replaced with WC. The inactive
hierarchy is built and walked before the PAT write; exact PAT readback precedes
the first `WBINVD`, then CR3 is loaded, then the second `WBINVD` runs. Rollback
keeps the corresponding reverse sequence. The framebuffer `sfence` remains in
the presentation path.

PAT selection is not the processor's complete effective-memory-type proof.
Firmware MTRRs combine with PAT, and a conflicting UC MTRR can dominate a WC
page selection. OpenSeneri still neither reads nor programs MTRRs, so bare-metal
effective type remains an explicit limitation.

## Proof structure

The pure paging self-test runs before PAT programming or CR3 replacement. Its
synthetic registries cover mixed WB/WC/UC input, reversed insertion order, zero
length, end overflow, capacity plus one, unknown kind, unknown memory type,
UC/WC and WB/WC conflicts, duplicate refusal, a framebuffer crossing a 2 MiB
boundary, multiple UC APICs, and absent optional windows.

After CR3 installation, `paging_verify_device_windows` first compares the
canonical expected collection with paging's installed copy. It then walks every
4 KiB page in every entry and requires successful translation, identity
physical/virtual addresses, requested semantic permissions, requested decoded
PAT type, and a level-1 leaf. W^X and user reachability remain separate whole-
hierarchy audits. A failure reports the window kind and the I/O APIC instance.

`prove_write_combining` stays an independent oracle: it derives ECAM and the
entire rounded framebuffer span again from the boot descriptions rather than
trusting the registry length. `screen_verify_cell` and the visual proofs still
read pixels from the physical framebuffer, never from the cached surface.

The `device-windows` scenario uses guest exit `0x2D`, host status 91. It walks
the installed complete registry, requires the policy table above, samples WB
RAM, and reports only stable counts:

```text
ST DEVICE-WINDOWS WINDOWS <n> PAGES <n> VGA 1 LOCAL-APIC 1 IO-APICS <n> ECAM <0|1> FRAMEBUFFER <0|1>
```

## Negative controls

Each control is applied to copied source files, clean-built, run through its
narrow and relevant full scenario, and restored from the copy. The completed
results are recorded here before merge.

| Control | Observed result |
| --- | --- |
| framebuffer requested as UC | `write-combining` and normal timed out in the deliberate halt with `PANIC: framebuffer range is not write-combining`; neither reached a scenario pass |
| framebuffer requested as WB | both runs produced the same framebuffer-range panic; WB was not accepted as WC |
| VGA requested as WC | `device-windows` and normal halted at `installed device-window proof failed: VGA` |
| local APIC requested as WC | both halted at `installed device-window proof failed: local APIC` |
| one I/O APIC requested as WC | both halted at `installed device-window proof failed: I/O APIC 0` |
| ECAM requested as WC on q35 | `device-windows` and `pci-ecam` halted at `installed device-window proof failed: PCI ECAM` |
| return the wrong status for the synthetic UC/WC overlap | the exact `CONFLICTING_DEVICE_WINDOW_OVERLAP` assertion failed before PAT access: `PANIC: page table arithmetic self-test failed` in narrow and normal boots |
| return the wrong status for the synthetic end overflow at `UINT64_MAX - 4096 + 2` | the exact `DEVICE_WINDOW_RANGE_OVERFLOW` assertion produced the same pre-PAT self-test panic in both boots |
| return the wrong status for capacity plus one | the exact `TOO_MANY_DEVICE_WINDOWS` assertion produced the same pre-PAT self-test panic in both boots |
| framebuffer entry truncated to one page | `write-combining` and normal halted at the independent `framebuffer range is not write-combining` proof |
| reverse every raw insertion before validation | both scenarios passed; q35 still reported 5 windows/1,283 pages and normal retained 11 table frames, 5 fine regions, and the same leaf counts |
| add a hidden `acpi_topology` read to paging | the kernel compiled, then `make verify` exited 2 at the source/API assertion before any QEMU boot |
| force usable ECAM to overlap I/O APIC 0 | the overlap was reported, ECAM became unavailable, the registry scenario reported `ECAM 0`, and the guest passed without a panic; the host target rejected only its q35-specific `ECAM 1` transcript contract |
| force the framebuffer span to overlap VGA | the memory-type conflict was reported, framebuffer availability was cleared, and normal boot passed on the serial-only path without a panic; the host target rejected only its normal graphics transcript contract |

The first attempt at the truncation control replaced the length expression and
made `page_end` unused; `-Werror` rejected that build before it could test the
runtime oracle. The control was restored and re-aimed by setting `page_end` to
the first-page boundary while leaving the production expression intact. That
clean-built version reached the independent proof and produced the result in
the table.

## Limitations and deferred work

- There is no live remapping or cache-type-change API.
- The registry covers only windows OpenSeneri currently touches; unowned MMIO holes
  remain in the bulk WB identity map.
- There is one ECAM selection and one framebuffer, matching current consumers.
- There is no alias audit for another virtual mapping of the same physical
  device page.
- MTRR inspection/programming, a higher half, huge-leaf splitting, userspace,
  per-process address spaces, and multiprocessor TLB shootdown remain deferred.
- KVM and bare-metal memory-type behavior require environments that expose
  those executors; QEMU cannot establish firmware MTRR behavior on real iron.

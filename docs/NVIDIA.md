<!-- SPDX-License-Identifier: GPL-3.0-only -->

# NVIDIA

## What this is, and what it is not

Sapote has fifteen bounded drivers for the register and configuration
contracts an NVIDIA graphics board publishes. They are written the same way the
other eighteen are: read the registers that carry the device's identity, check
what came back against a property the documentation guarantees, refuse the
device rather than report whatever was found, and give everything back.

Not all fifteen are equally NVIDIA-specific, and four of them read PCI
capabilities every function on the bus carries. They are here as cross-checks
on the ones that are, not as vendor knowledge. [Which of these are NVIDIA
drivers, honestly](#which-of-these-are-nvidia-drivers-honestly) says which is
which, driver by driver.

**None of it has been run against NVIDIA silicon.** No NVIDIA device was
reachable from the machine this was written on, and no emulator Sapote is
tested under models one. That is stated here, in `include/sapote/nvidia.h`, in
the module comment of `src/kernel/nvidia.c`, and by a `make verify` gate that
refuses a build in which the statement has been removed. It is the single most
important thing on this page.

It has been run against a *model* of the register interface, which is a
different and lesser thing — see [Against a device model](#against-a-device-model)
below. The model raises these drivers from "never executed" to "executed end to
end against a stand-in", and it cannot raise them further.

What *is* proved, on every boot and in every `make verify`, is everything that
does not need the device:

- the identity decode against the published encoding, thirteen values wide;
- the VBIOS parser against a reference image stated three independent times;
- the refusal of every function that is not NVIDIA's, with display and HD Audio
  functions of exactly the matching classes attached so the refusal has
  something to refuse;
- a resource census identical afterwards, and nothing claimed, mapped or left
  bus-mastering.

## Where the material comes from

NVIDIA publishes no register manual for its graphics parts. What exists instead
is a body of public material that is, in aggregate, better evidence than one
datasheet, because several independent efforts agree:

| Source | What it settles here |
| --- | --- |
| [envytools](https://github.com/envytools/envytools) | register names, offsets and areas: PMC at 0, PTIMER at 0x9000, PROM at 0x300000 |
| Nouveau (Linux `drivers/gpu/drm/nouveau`) | the `PMC_BOOT_0` decode, the family table, the ROM shadow sequence, the carry-safe timer read |
| Mesa / NVK | architecture naming and generation boundaries |
| NVIDIA open GPU kernel modules and published headers | corroboration of the same offsets on modern parts |
| PCI Firmware Specification 3.0 §5.1 | the expansion ROM header and PCIR data structure |
| PCI Local Bus Specification 3.0 §6.2.5.2, §6.7, §6.8.1 | the expansion ROM base address register, capability list alignment, and MSI message control |
| PCI Bus Power Management Interface Specification 1.2 §3.2 | the power-management capability's version and power state |
| PCI Express Base Specification | the capability register's type and version, device capabilities and device control |
| HD Audio 1.0a §3.3 | the audio function's version and stream counts |

Where that record is exact, the driver states it. Where it is not, the driver
reads and reports rather than asserting. The BIT header's bytes at +6 through
+8 are an example: no consumer here reads them, so the parser reports the
structure without inventing a meaning for them.

## The fifteen drivers

| # | Driver | Access | What it establishes |
| --- | --- | --- | --- |
| 0 | GPU master control | BAR0 | `PMC_BOOT_0` decodes to a known architecture, and `PMC_BOOT_1` says the aperture is little-endian |
| 1 | GPU configuration mirror | BAR0 | the mirrored first configuration dword equals the one enumeration read through configuration cycles |
| 2 | GPU timer | BAR0 | `PTIMER`'s 64-bit count advances across a bounded wait |
| 3 | GPU video BIOS | BAR0 | the PROM window holds a PCI expansion ROM whose PCIR names NVIDIA, with an NVIDIA BIT table inside it |
| 4 | HD Audio function | BAR0 | the audio function answers HD Audio version 1.0 with at least one output stream |
| 5 | GPU boot straps | BAR0 | `PEXTDEV_BOOT_0` answers, and differently from `PMC_BOOT_0` |
| 6 | GPU master control engines | BAR0 | `PMC_ENABLE` and the two interrupt registers are distinct registers, not one value mirrored across the window |
| 7 | GPU memory apertures | BAR sizes | the register window is non-prefetchable and the framebuffer aperture beside it is prefetchable and no smaller |
| 8 | GPU PCI Express link | config | the Express capability describes a negotiated link no wider than the one it advertises |
| 9 | GPU board identity | config | the subsystem identity names a real add-in-board partner, over a chip whose vendor is still NVIDIA |
| 10 | GPU power management | config | the power-management capability is present, its version is one the specification defines, and the function is in D0 |
| 11 | GPU message interrupts | config | the MSI capability's counts are legal encodings and nothing has enabled it |
| 12 | GPU PCI Express endpoint | config | the Express capability identifies itself, has a version, and describes an endpoint whose programmed payload is within what it supports |
| 13 | GPU expansion ROM declaration | config | the ROM decoder this board declares to PCI is switched off and its reserved bits read as zero |
| 14 | GPU timer scale | BAR0 | the timer's rate registers are non-degenerate and hold still while the counter beside them moves |

### Which of these are NVIDIA drivers, honestly

**Nine are NVIDIA-specific.** Drivers 0 through 7 and 14 read registers that
only NVIDIA-specific documentation describes, at offsets that mean nothing on
any other vendor's part. Driver 3 goes further: the BIT table it finds inside
the ROM is NVIDIA's own structure.

**Four are generic.** Drivers 8, 10, 11 and 12 read PCI and PCI Express
capabilities that **every** function on the bus carries. What earns them a
place here is not vendor knowledge but cross-checking: driver 10 asks the
function what power state it is in, and the only answer consistent with drivers
0 through 7 having just read its memory-mapped registers is D0, because every
lower state turns that decoder off. Driver 12 refuses a function that answers
to the display class while describing itself as a port.

**Two are in between.** Driver 9 reads a generic register — the subsystem
identity — whose meaning for a graphics board is not generic at all. Driver
13's expansion ROM checks are generic PCI, but what it cross-checks them
against is the NVIDIA ROM driver 3 read through the PROM aperture.

Calling all fifteen "NVIDIA drivers" without saying that would be inflation.

Driver 1 is the strongest identification available here. The graphics function
mirrors its own PCI configuration space into the register aperture — at
0x001800 before NV50 and 0x088000 from NV50 on — so the first dword of that
mirror has to be the same vendor and device the enumeration read through
configuration cycles. One source could be anything. Two sources that agree,
reached through completely different hardware paths, are the device stating its
identity twice.

Driver 0 runs first because drivers 1 and 3 pick their register offsets from
the architecture it establishes. Four other drivers depend on an earlier one
having run: 6 on 0, 13 on 3, and 14 on 2. The table's order is those
dependencies, and a foundation control states each pair by name, so a table
reordered into an order that breaks one of them is refused at boot rather than
silently producing a weaker result.

## The one write

Fourteen of the fifteen drivers write nothing at all. The fifteenth has to: the PROM
window answers with a shadow copy until the shadow bit in the configuration
mirror is cleared, so the video BIOS driver reads that register, clears one
bit, reads the window, puts the original value back, and reads it again to
prove it went back. A driver that leaves a device in a state it did not find it
in has not finished. `make verify` refuses a build in which those four lines
appear in any other order, or in which any driver here gains a second write.

No driver enables bus mastering or allocates DMA. Sapote has no IOMMU, so a
driver that cannot reach memory is the difference between one that cannot
corrupt the kernel and one that is merely not expected to.

## The identity decode

`PMC_BOOT_0` is the first register on the part and the one every driver for
this hardware has read first since 1999. Nouveau keys its whole device table
off it:

```
chipset  = (boot0 & 0x1ff00000) >> 20
revision =  boot0 & 0x000000ff
family   =  chipset & 0x1f0
```

The family boundaries are Nouveau's own, not marketing names, which is why a
part this kernel has never heard of still lands in the right generation:

| family | architecture | | family | architecture |
| --- | --- | --- | --- | --- |
| 0x010 | Celsius | | 0x0e0/0x0f0/0x100 | Kepler |
| 0x020 | Kelvin | | 0x110/0x120 | Maxwell |
| 0x030 | Rankine | | 0x130 | Pascal |
| 0x040/0x060 | Curie | | 0x140 | Volta |
| 0x050/0x080/0x090/0x0a0 | Tesla | | 0x160 | Turing |
| 0x0c0/0x0d0 | Fermi | | 0x170 | Ampere |
| | | | 0x190 | Ada |

Two readings are refused rather than decoded, and both matter more than the
ones that succeed: `0x00000000` is an aperture that is not there, and
`0xFFFFFFFF` is a bus answering with all ones. Neither is a family, so neither
becomes a part. A family the table does not carry — 0x180, say — is *unknown*,
never the nearest one.

The `nvidia` and `nvidia-builtin` scenarios re-derive thirteen encodings
independently of the driver's own table, so a decode that drifted would have to
drift the same way in two places.

## The video BIOS

Bytes out of a board's ROM are bytes from outside, which makes them Rust's job
rather than C's. `src/rust/nvbios.rs` validates them and C never parses one.

What it checks is exactly what a consumer would otherwise trust: that every
offset the image points at is inside the image. The 0xAA55 signature, the PCIR
pointer at 0x18, the "PCIR" structure and its vendor, the declared image length
in 512-byte blocks, the x86 code type, then NVIDIA's BIT table — identifier
0xB8FF little-endian followed by `BIT\0`, which is the byte sequence
`FF B8 42 49 54 00` that Nouveau searches for — and every one of its tokens,
whose declared regions must fall inside the image.

Sixteen controls run on the host under `make verify` and again inside the
kernel on every boot. Each takes the reference image, breaks exactly one
structural field, and requires the named refusal. A parser that accepted any of
them would be trusting an offset it had not checked.

The reference image is **synthesised, not dumped**: no board's ROM is
reproduced anywhere in this repository, and the image's PCIR device identifier
is `0x5341` precisely so it can never be mistaken for a real part. It is stated
three independent times — as a C table in `src/kernel/nvidia.c`, as Rust in
`src/rust/nvbios.rs`, and as a Python record in `tools/nvidia_vbios_image.py`
— and the build compares all three. Any two disagreeing is a build failure
rather than a parser that quietly accepts something else.

## Evidence

`nvidia` attaches a standard VGA adapter, a Cirrus adapter, a Bochs display and
an Intel HD Audio controller — display and multimedia functions of exactly the
classes these drivers match on, from four vendors, none of them NVIDIA. It
requires:

- all twenty-one foundation controls passed;
- at least three functions of a matching class present, and every one refused;
- no NVIDIA function invented: nothing present, nothing bound, no register read,
  no register written, no identity decoded;
- the Boot Ledger receipt for the probe, its two proof counters, and the
  capability set that goes with a probe that ran;
- the frame, paging, DMA, PCI, vector and MSI-X census identical to before.

`nvidia-builtin` runs on a bare machine and requires the opposite ledger shape:
the foundation stage ran, the probe stage was *skipped*, the skip is recorded
with its reason, and nothing was claimed or mapped at any point.

The line the `nvidia` scenario produces on this machine, and the only one it
has ever produced anywhere:

```
ST NVIDIA declared 15 present 0 bound 0 controls 21 architecture unknown
   chipset 0x0 reads 0 writes 0 no function present teardown clean census equal
```

## Against a device model

`tools/qemu/sapote_nvidia_model.c` is a QEMU device that puts vendor `0x10DE`
on the PCI bus with the registers, capabilities and apertures these drivers
read. Built into QEMU 8.2.2, it lets the bind path execute: real PCI
enumeration, a real claim through the typed substrate, a real BAR mapping, real
MMIO through the page tables, real configuration cycles, and a real teardown.

Run against it, with the boot register, the straps, the board identity and the
timer's rate pair pinned on the command line and the ROM image generated by the
independent Python record:

```
ST NVIDIA driver  0 GPU master control          bound 0x164000A1/0x0          reads    2 writes 0
ST NVIDIA driver  1 GPU configuration mirror    bound 0x1B8010DE/0x1B8010DE   reads    2 writes 0
ST NVIDIA driver  2 GPU timer                   bound 0xD10D0969/0x200FF3     reads    6 writes 0
ST NVIDIA driver  3 GPU video BIOS              bound 0x10DE5341/0x400        reads 8194 writes 2
ST NVIDIA driver  4 HD Audio function           absent
ST NVIDIA driver  5 GPU boot straps             bound 0x42C/0x164000A1        reads    2 writes 0
ST NVIDIA driver  6 GPU master control engines  bound 0x1111111100000000/0x0  reads    4 writes 0
ST NVIDIA driver  7 GPU memory apertures        bound 0x10000000/0x1000000    reads    0 writes 0
ST NVIDIA driver  8 GPU PCI Express link        bound 0x11/0x411              reads    2 writes 0
ST NVIDIA driver  9 GPU board identity          bound 0x87651043/0x1B8010DE   reads    2 writes 0
ST NVIDIA driver 10 GPU power management        bound 0x30001/0x0             reads    2 writes 0
ST NVIDIA driver 11 GPU message interrupts      bound 0x80/0x50               reads    1 writes 0
ST NVIDIA driver 12 GPU PCI Express endpoint    bound 0x8000/0x9200000000     reads    3 writes 0
ST NVIDIA driver 13 GPU expansion ROM decl.     bound 0x0/0x400               reads    1 writes 0
ST NVIDIA driver 14 GPU timer scale             bound 0x1B000003E8/0x208E51   reads    9 writes 0
ST NVIDIA declared 15 present 14 bound 14 controls 21 architecture Turing
   chipset 0x164 reads 8230 writes 2 function present teardown clean census equal
ST PASS nvidia
```

Fourteen drivers bound; the audio one correctly reported absent, because the
model presents no audio function. Read across the numbers:

- driver 0 decoded `0x164000A1` — pinned on the command line, not known to the
  driver — into **Turing, chipset 0x164**;
- driver 1's two values are identical, which is the mirror and the enumeration
  agreeing through different hardware paths;
- driver 2's second value is the delta: `0x200FF3` is 2,101,235 ns across a
  2 ms window, so the counter genuinely advanced;
- driver 3 read `0x10DE5341` out of the ROM — vendor `0x10DE`, device `0x5341`,
  the synthesised marker the Python record puts there — and `0x400` is the
  1,024-byte image length. Its 8,194 reads are the ROM prefix plus the shadow
  register, and its **2 writes** are the shadow bit cleared and restored;
- driver 5 read back the strap value pinned on the command line, and its second
  value is `PMC_BOOT_0` — proof the aperture answered two offsets differently;
- driver 7's **zero reads** are the point: it establishes the aperture shape
  from BAR descriptions without touching the device at all, and the scenario
  asserts that count is exactly zero;
- driver 8 found a real PCI Express capability, so `0x11` is a genuinely
  negotiated link rather than a fabricated answer;
- driver 9 read back the subsystem identity pinned on the command line, and its
  second value is the chip's own vendor and device;
- driver 10's `0x30001` is the power-management capability header: identifier
  `0x01`, capabilities `0x0003` — interface version 3, which is PCI PM 1.2 —
  and its second value is the control register, `0` for D0;
- driver 11's `0x80` is the MSI message control: 64-bit address capable, enable
  clear, and both count fields zero;
- driver 12's `0x8000` is the device capabilities register and its second value
  packs the Express capability register above the device control register.
  `0x92` decodes as **type 9, version 2** — a root complex integrated endpoint,
  which is what a device on q35's root bus is;
- driver 13 read the expansion ROM base address register: `0` — no ROM
  declared, decoder off — and its second value is the 1,024-byte image driver 3
  did read, through the PROM aperture instead;
- driver 14 read the rate pair `27/1000` back exactly as pinned on the command
  line, and its second value is the counter's advance while that pair held
  still.

### The same board behind a root port

Placed behind a `pcie-root-port` instead of on the root bus, driver 12's
Express capability register reads `0x0002` — **type 0, a plain endpoint** —
rather than `0x0092`. Both bind, which is the correct outcome: the
specification gives endpoints three encodings, and the check refuses only what
is not an endpoint at all.

### An alignment bug the model exposed

Adding the power-management and message-interrupt capabilities to the model
broke drivers 8 and 12, and the reason was worth keeping. QEMU's automatic
capability placement packs capabilities byte-tight, and with two more in front
of it the Express capability landed at offset `0x5E` — not dword aligned. PCI
requires those pointers to be dword aligned; Sapote's enumeration masks the
bottom two bits off, as it should, and so reached `0x5C`, where the bytes
belong to the capability before it. Driver 8 read a garbage link description
and passed on it.

Two fixes, both worth having:

- the model now states all three capability offsets explicitly, because real
  parts are aligned and a model that is not tests the wrong thing;
- driver 12 now reads the capability's own identifier byte back from the device
  before decoding anything out of it, so an unreachable capability becomes a
  refusal with a name instead of a field decoded out of a neighbour.

### Eleven controls prove the run is not vacuous

Six of them change a value the model answers with; five inject a named defect.

| Model change | Refusal |
| --- | --- |
| `boot0=0x180000A1` (a family the table does not carry) | *NVIDIA device identity was refused* |
| `straps` equal to `boot0` | *NVIDIA register aperture answers every offset alike* |
| `straps=0xFFFFFFFF` (an open bus) | *NVIDIA board straps were refused* |
| `subsystem=0` (no board identity) | *NVIDIA device identity was refused* |
| `enable=0xFFFFFFFF` (an open bus) | *NVIDIA device identity was refused* |
| one byte of the ROM's PCIR vendor | *NVIDIA video BIOS image is malformed* |
| `numerator=0` | *NVIDIA timer rate registers are degenerate* |
| `denominator=0xFFFFFFFF` | *NVIDIA timer rate registers are degenerate* |
| `rom=0x00000001` (ROM decode enabled) | *NVIDIA expansion ROM declaration was refused* |
| `rom=0x00000400` (a reserved bit set) | *NVIDIA expansion ROM declaration was refused* |
| `defects=0x1` (no power-management capability) | *NVIDIA power management capability was refused* |
| `defects=0x2` (message interrupts already enabled) | *NVIDIA message interrupts are not in the state this kernel left them* |
| `defects=0x4` (calls itself a root port) | *NVIDIA function is not a PCI Express endpoint* |
| `defects=0x8` (the rate register aliases the counter) | *NVIDIA timer rate registers are degenerate* |

The last two defects are told on the wire rather than written into QEMU's own
configuration space: a device reset would undo a written one before the guest
looked, and rewriting the Express type in place makes QEMU's own PCI Express
code assert. A device that lies only when asked is also the more faithful
model of a device that lies.

Two further controls check the *checks* rather than the device:

- with `defects=0x8` set and driver 14's re-read of the numerator removed, the
  run **passes** — so the re-read is what catches the aliased register, not
  something else;
- moving driver 14 ahead of driver 2 in the table makes the foundation
  self-test refuse before any binding happens: *bounded NVIDIA controls
  failed*, and the Boot Ledger refuses the boot. The ordering is enforced, not
  documented.

One assertion in driver 13 is deliberately **not** claimed as a control: that
the image driver 3 produced is a whole number of 512-byte blocks. The Rust
validator derives that length from the image's own block count, so no device
can make it fire. It is a boundary re-assertion at the point C first uses a
number that crossed from another language, and it is described as one.

### What the model cannot tell you

The register offsets in the model and in the driver come from the same reading
of the same documents. A misreading would appear identically in both, and the
model would confirm it. Only silicon settles that. The model's authority is
limited to what it does *not* decide — the boot value, the ROM bytes, the
configuration mirror and the clock all come from outside it.

The model is also not part of `make verify` or the 101 scenarios: it needs a
QEMU built from source, so it is reproducible evidence rather than a gate.

### A bug found on the way, and fixed

Building a newer QEMU turned up something unrelated to NVIDIA: **Sapote used to
panic on QEMU 9.1.0 while booting fine on 8.2.2.** On 9.1 the RFLAGS image saved
on some CPL3 traps has RF (bit 16) set, and every one of this kernel's four user
boundaries required it clear:

```
DIAG branchC cs 0x33 rfl 0x00010046 tail 1 rsp 0x400000205000 ss 0x2B
Sapote PANIC: required stage failed (installed multiprocess proof)
```

The mistake was categorical rather than numerical. RF is not a flag a program
sets; it is the processor's own note about the trap, and both emulators are
entitled to their answer. A kernel authenticating a user register set has to
decide what to do with a bit the user did not choose, and requiring it clear is
the wrong answer — it refuses a legal return.

`CPU_RFLAGS_PROCESSOR_BOOKKEEPING` in `include/sapote/cpu.h` now names that
distinction, and all four boundaries **discard** the bit rather than tolerate
it: the Ring 3 proof, the multiprocess trap, the multiprocess saved context and
the Linux syscall boundary. Discarding matters more than tolerating — a saved
context is normalised on the way in, so nothing hands the bit back to a process
through an IRETQ, and every other flag outside the arithmetic set is still
required to be exact. `make verify` refuses a tree where a boundary forgets.

Sapote now boots on both, and the NVIDIA drivers bind on both:

```
QEMU 9.1.0: ST MULTIPROCESS processes 4 rounds 6 switches 28 ... fault contained
            ST NVIDIA declared 15 present 14 bound 14 ... architecture Turing
            ST PASS nvidia
```

## Limits

Beyond the one that matters most — no hardware has run this, only a model —
the bounds are:

- **Fifteen read-only contracts, not a graphics driver.** There is no mode
  setting, no framebuffer programming, no display link, no channel, no
  pushbuffer, no command submission, no engine, no firmware load, no interrupt,
  and no memory management. Driver 10 *reads* the power-management capability;
  it does not manage power, and nothing here changes a power state. Nothing
  here can put a pixel on screen.
- **The graphics function and the audio function only.** The USB xHCI and
  USB Type-C UCSI functions Turing and later boards expose are not covered.
  Sapote already drives xHCI properly, and a bounded lesser duplicate of a
  driver the kernel already has is worse than none.
- **Four of the fifteen are generic PCI capability reads.** Drivers 8, 10, 11
  and 12 would work against any vendor's function; their value here is as
  cross-checks on the NVIDIA-specific ones, not as vendor knowledge.
- **A bounded prefix of the ROM.** Eight kilobytes of the PROM window are read.
  An image whose BIT table lies beyond that is reported as malformed rather
  than searched for further.
- **The probe does not run on ordinary boots.** Binding claims a live graphics
  function and writes one bit of it; on a machine whose display this kernel is
  already drawing on, that is not something to do uninvited. The foundation
  stage runs everywhere; the probe runs where its scenario asks for it.
- **BIT only.** Boards old enough to carry a BMP structure instead are refused
  rather than half-parsed.

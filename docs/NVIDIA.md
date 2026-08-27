<!-- SPDX-License-Identifier: GPL-3.0-only -->

# NVIDIA

## What this is, and what it is not

Sapote has five bounded drivers for the register contracts an NVIDIA graphics
board publishes. They are written the same way the other eighteen are: read the
registers that carry the device's identity, check what came back against a
property the documentation guarantees, refuse the device rather than report
whatever was found, and give everything back.

**None of it has been run against NVIDIA silicon.** No NVIDIA device was
reachable from the machine this was written on, and no emulator Sapote is
tested under models one, so the bind path is code that has never executed
against the hardware it describes. That is stated here, in
`include/sapote/nvidia.h`, in the module comment of `src/kernel/nvidia.c`, and
by a `make verify` gate that refuses a build in which the statement has been
removed. It is the single most important thing on this page.

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
| HD Audio 1.0a §3.3 | the audio function's version and stream counts |

Where that record is exact, the driver states it. Where it is not, the driver
reads and reports rather than asserting. The BIT header's bytes at +6 through
+8 are an example: no consumer here reads them, so the parser reports the
structure without inventing a meaning for them.

## The five drivers

| # | Driver | Function | What it establishes |
| --- | --- | --- | --- |
| 0 | GPU master control | display | `PMC_BOOT_0` decodes to a known architecture, and `PMC_BOOT_1` says the aperture is little-endian |
| 1 | GPU configuration mirror | display | the mirrored first configuration dword equals the one enumeration read through configuration cycles |
| 2 | GPU timer | display | `PTIMER`'s 64-bit count advances across a bounded wait |
| 3 | GPU video BIOS | display | the PROM window holds a PCI expansion ROM whose PCIR names NVIDIA, with an NVIDIA BIT table inside it |
| 4 | HD Audio function | multimedia | the audio function answers HD Audio version 1.0 with at least one output stream |

Driver 1 is the strongest identification available here. The graphics function
mirrors its own PCI configuration space into the register aperture — at
0x001800 before NV50 and 0x088000 from NV50 on — so the first dword of that
mirror has to be the same vendor and device the enumeration read through
configuration cycles. One source could be anything. Two sources that agree,
reached through completely different hardware paths, are the device stating its
identity twice.

Driver 0 runs first because drivers 1 and 3 pick their register offsets from
the architecture it establishes.

## The one write

Four of the five drivers write nothing at all. The fifth has to: the PROM
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
classes the five drivers match on, from four vendors, none of them NVIDIA. It
requires:

- all fourteen foundation controls passed;
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
ST NVIDIA declared 5 present 0 bound 0 controls 14 architecture unknown
   chipset 0x0 reads 0 writes 0 no function present teardown clean census equal
```

## Limits

Beyond the one that matters most — no hardware has run this — the bounds are:

- **Five register contracts, not a graphics driver.** There is no mode setting,
  no framebuffer programming, no display link, no channel, no pushbuffer, no
  command submission, no engine, no firmware load, no power management, no
  interrupt, and no memory management. Nothing here can put a pixel on screen.
- **The graphics function and the audio function only.** The USB xHCI and
  USB Type-C UCSI functions Turing and later boards expose are not covered.
- **A bounded prefix of the ROM.** Eight kilobytes of the PROM window are read.
  An image whose BIT table lies beyond that is reported as malformed rather
  than searched for further.
- **The probe does not run on ordinary boots.** Binding claims a live graphics
  function and writes one bit of it; on a machine whose display this kernel is
  already drawing on, that is not something to do uninvited. The foundation
  stage runs everywhere; the probe runs where its scenario asks for it.
- **BIT only.** Boards old enough to carry a BMP structure instead are refused
  rather than half-parsed.

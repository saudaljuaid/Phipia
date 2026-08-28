<!-- SPDX-License-Identifier: GPL-3.0-only -->

# High Definition Audio

Sapote's thirteen bounded drivers read registers and nothing else, on purpose:
a driver that never enables bus mastering is a driver that cannot reach memory,
and on a machine with no IOMMU that is the difference between *cannot* and *is
not expected to*. This one is different. High Definition Audio has no register
a driver can ask a codec through. It has two rings in memory — the controller
reads commands out of one and writes the codecs' answers into the other, both
by bus-mastering DMA — so talking to a codec at all means letting the device
write into kernel memory.

That is the interesting part of this driver, and the ordering that makes it
safe is most of the file.

## The rule this driver exists to follow

```
allocate the rings as typed DMA allocations
    refuse bus mastering while the rings still belong to this side   <- proved
    hand the rings to the device
    enable bus mastering, naming exactly those two allocations
        start the ring engines
        ... the conversation ...
        stop the ring engines
    put the controller back into reset
withdraw bus mastering
    take the rings back from the device
release the memory the rings were
```

Every line of that is checked. The refusal is a *proof*, not a comment:
`pci_claim_enable_bus_master` is called once before the rings are transferred
and is required to answer `DMA_NOT_PREPARED`, and only then called again for
real. `make verify` refuses a build in which the teardown lines appear in any
other order, and the scenario refuses a boot in which anything is still
allocated, claimed or bus-mastering afterwards.

The controller reset before bus mastering is withdrawn is not redundant with
stopping the engines. Stopping an engine is a request; a controller in reset
has no engines.

## What the conversation is

Software writes a *verb* — a codec address, a node number and a command — into
the command ring and advances the write pointer. The controller reads it, puts
it on the link, and writes whatever the codec answers into the response ring,
advancing its own write pointer. Reading that pointer is how a driver knows an
answer arrived, and it is the only thing in this driver that waits.

For every codec the controller reports on the link, Sapote asks four questions:

| Verb | What it establishes |
| --- | --- |
| `GET_PARAMETER(VENDOR_ID)` | who made this codec and which part it is |
| `GET_PARAMETER(REVISION_ID)` | which revision of it |
| `GET_PARAMETER(SUBORDINATE_NODE_COUNT)` | where its function groups are numbered |
| `GET_PARAMETER(FUNCTION_GROUP_TYPE)` | whether the first of them is an audio function group |

Every answer is authenticated before it is believed: the extended half of a
response carries the address of the codec that sent it, and a response from the
wrong codec, or one no command asked for, is refused rather than recorded. An
overrun in the response status — the controller having written past what this
side had read — is a lost answer rather than a late one, and is also refused.

Sapote polls the response ring rather than taking the response interrupt, so it
sets the interrupt threshold beyond the number of answers one conversation
collects *and* acknowledges the response flag after each answer anyway. A
threshold is a bound, not a promise.

## Evidence

The `audio` scenario attaches an ICH9 HD Audio controller and one codec, and
requires:

- the controller to leave reset and report a version 1 interface with at least
  one output stream;
- at least one codec on the link, and every codec present to be identified;
- as many responses as verbs, every one of them from the codec it was asked of;
- an audio function group among them;
- bus mastering withdrawn before the rings were released;
- the frame, paging, DMA, PCI, vector and MSI-X census identical to before, and
  no allocation, claim or bus master left behind.

A codec identity cannot be produced from this side of the link. The one QEMU
presents answers `0x1AF40022` — vendor 0x1AF4, device 0x0022 — at revision
`0x00100101`, with its function group at node 1.

## Limits

This driver identifies codecs. It does not play anything.

There is no stream descriptor, no buffer descriptor list, no format
negotiation, no widget graph walk, no mixer, no volume, no input capture, and
no interrupt. Adding playback means a third DMA allocation the device reads
continuously rather than once, which is a larger claim about DMA ownership than
this increment makes, and it needs its own evidence — a link position counter
that advances is the honest proof, and it is not in this release.

The driver is also bounded to one controller and to the codecs that controller
reports at bind time. There is no hotplug, no unsolicited response handling,
and no power management.

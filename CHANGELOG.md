<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Changelog

## 2.2.0 — Several Processes and Thirteen Bounded Drivers

- Replaced the one-process-ever address-space model with up to four private
  hierarchies live at once, each with its own image, stack, generation and
  saved CPL3 register set.
- Added a bounded round-robin scheduler over those processes, a saved-context
  Ring 3 entry, a re-armable proof gate, and per-resume and per-trap
  authentication of the whole user register set.
- Added contained user faults: a process that stores into its guard page is
  terminated while its neighbours run to completion and everything is released
  together.
- Added an ordering rule for private identity-alias restores so one process's
  teardown cannot free a page table another still has a leaf in.
- Added a second admitted ELF64 profile, its instruction stream pinned
  independently in the kernel, in freestanding Rust, and in a Python record
  that `make verify` compares against the kernel's table.
- Added thirteen bounded drivers for real Intel, Realtek, AMD, Cirrus Logic and
  Bochs Display Interface devices, each binding through the typed PCI
  substrate, resetting where its specification defines a reset, and identifying
  its device against a property that specification guarantees.
- Added station addresses pinned on the host command line and required back out
  of four of those drivers, so a bind cannot be satisfied by a plausible value
  the driver did not fetch.
- Added an HD Audio driver that identifies every codec on the link over
  bus-mastering command and response rings, refusing bus mastering before the
  rings are prepared and withdrawing it before they are reclaimed, with the
  teardown order enforced by the build.
- Added a TCP passive open: `LISTEN` and `SYN_RECEIVED` states, a declared
  backlog bounded at four, `network_tcp_listen` and `network_tcp_accept`,
  listener-owned children reclaimed on close, and readiness reporting for a
  connection waiting to be accepted.
- Added a reset for TCP segments matching no connection and no listener, with
  RFC 793 section 3.4 sequence numbers and no reset in answer to a reset.
- Fixed a remote-triggerable buffer reuse: a handler answering the frame it was
  parsing could reach an ARP wait that pumped the device again, overwriting the
  receive and transmit buffers its own caller held. The pump now refuses
  recursive entry and an unresolved send from the receive path defers to
  retransmission instead.
- Added five bounded NVIDIA drivers written from envytools, Nouveau, Mesa/NVK,
  NVIDIA's open kernel modules and the PCI Firmware Specification: the master
  control identity decode, the configuration-space mirror cross-check, the
  timer, the video BIOS window and the HD Audio function. Exactly one of them
  writes a register, and it proves the write reversed.
- Added a freestanding Rust VBIOS validator with sixteen controls, and a
  synthesised reference image stated three independent times in C, Rust and
  Python with the build comparing all three.
- Added eight typed Boot Ledger stages, nine QEMU scenarios and a 101-scenario
  total contract.

Explicit non-features: no preemptive user scheduling, no fork, exec, signals,
process identifiers or inter-process communication; the thirteen bounded
drivers move no data, enable no bus mastering, allocate no DMA and take no
interrupt; the HD Audio driver identifies codecs and plays nothing; a TCP
listener has no background retransmission timer, no listen queue that outlives
its caller, and no rate limit on refusals; the five NVIDIA drivers have never
run against NVIDIA silicon, read five register contracts rather than driving a
graphics part, and do no mode setting, framebuffer programming, command
submission or memory management.

## 2.1.0 — Advanced Networking and Browser Foundation

- Added a modern virtio-net PCI/MSI-X/DMA driver with bounded queues, explicit
  ownership and complete reset teardown.
- Added validated Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP and HTTP/1.1,
  including streamed synchronized FAT32 downloads and recovery-safe replace.
- Added authenticated version-1 native networking syscalls, readiness,
  cancellation, monotonic time and bounded entropy with degraded-state
  reporting.
- Added Terminal networking commands and a typed Boot Ledger networking stage.
- Added a deterministic offline Ethernet peer, PCAP reconstruction, negative
  modes, 34 new production QEMU scenarios and a 92-scenario total contract.
- Added an authentic interactive networking screenshot/video/PCAP bundle,
  a concrete NetSurf port plan, and a TLS prerequisite evaluation.

Explicit non-features: no browser, Chromium, browser icon, JavaScript, TLS,
HTTPS, IPv6, firewall, Wi-Fi, physical-NIC support, or secure-Internet claim.

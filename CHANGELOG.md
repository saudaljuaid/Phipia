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
- Added four typed Boot Ledger stages, four QEMU scenarios and a 96-scenario
  total contract.

Explicit non-features: no preemptive user scheduling, no fork, exec, signals,
process identifiers or inter-process communication; no driver here moves data,
enables bus mastering, allocates DMA, or takes an interrupt.

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

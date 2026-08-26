<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Networking

Sapote 2.1.0 has a bounded IPv4 networking foundation for one modern
`virtio-net-pci` device under QEMU. Packets cross the normal PCI claim, mapped
BAR, MSI-X, split virtqueue, DMA-ownership, protocol, syscall or Terminal, and
FAT32/NVMe paths. The deterministic peer is a host-side Ethernet endpoint; it
does not inject results into private kernel helpers.

This is not an Internet-security claim. Sapote has no IPv6, TLS, firewall,
routing, Wi-Fi, physical-NIC support, or browser.

## Device contract

Only PCI ID `1af4:1041` is accepted. The driver requires the modern PCI
capability layout, `VIRTIO_F_VERSION_1`, MAC and status features, at least two
queues, and MSI-X. Legacy/transitional transport, mergeable receive buffers,
offloads, multiqueue, control queues, and indirect descriptors are refused.

| Resource | Bound |
| --- | ---: |
| NICs | 1 |
| RX/TX queue descriptors | 16 / 16 |
| RX/TX packet reserves | 32 / 16 |
| packet arena | 48 × 2,048 bytes |
| accepted Ethernet frame | 1,514 bytes |
| MSI-X entries used | 1 |

Every packet has an explicit owner. Reset first stops the device, disables bus
mastering, unbinds MSI-X, returns DMA to the CPU, releases the allocations and
PCI claim, invalidates sockets and caches, and advances the device generation.
Stale handles cannot alias a later device generation.

## Protocol contract

- Ethernet II accepts only the configured unicast MAC, broadcast, and required
  IPv4 multicast forms. Unsupported EtherTypes and malformed lengths are
  counted and dropped.
- ARP has eight authenticated entries, three 500 ms attempts, a 60 s lifetime,
  conflict detection, and generation invalidation.
- IPv4 validates version, IHL, total length, TTL, header checksum, destination,
  and fragmentation flags before dispatch. Fragment reassembly is absent.
- ICMP implements bounded echo request/reply and reports timeouts. UDP validates
  pseudo-header checksums when present; IPv4 UDP zero-checksum datagrams are
  accepted as the protocol permits.
- DHCP performs DISCOVER/OFFER/REQUEST/ACK with three bounded attempts. It
  validates transaction, client identity, message type, server identity,
  subnet/router/DNS values, lease, renewal, and rebinding options. NAK and
  timeout leave no partial configuration.
- DNS supports bounded A and CNAME resolution, compression pointers with a
  16-pointer loop bound, four CNAME follows, 512-byte messages, eight cached
  entries, negative answers, TTL expiry, and configuration/device generations.
- TCP provides eight active connections, 8,192 receive bytes and one 1,460-byte
  retransmission segment per connection, four retransmissions, checked sequence
  and acknowledgement state, FIN close, RST handling, polling, cancellation,
  and owner/generation isolation. It is deliberately not a full RFC-complete
  congestion-control implementation.
- HTTP/1.1 accepts `http://` URLs only. It bounds headers to 4,096 bytes and 32
  fields, supports `Content-Length`, chunked transfer, and four redirects, and
  rejects conflicting framing, malformed chunks/status/header lines, redirect
  loops, unsupported schemes, truncation, and bodies above 16 MiB.

HTTP downloads use a temporary FAT32 path and synchronized replacement. A
failed transfer removes its temporary state; the previous destination remains
intact. Nested 8.3 paths, full-media refusal, clean reboot persistence, and the
immutable system volume are QEMU-tested.

## Public kernel and syscall bounds

`include/sapote/network.h` is native ABI version 1. It exposes explicit owners,
generation-authenticated handles, deadlines, readiness and cancellation. The
global bounds are eight UDP sockets, eight TCP connections, 32 timers, eight
poll handles per call, four queued datagrams per UDP socket, and 512 bytes per
datagram.

`include/sapote/network_syscall.h` is an experimental Sapote-private ABI version
1 for future native processes. At most four authenticated process contexts may
exist. A request transfers at most 4,096 bytes, random requests at most 256
bytes, and any deadline at most 30 seconds. Before the first copy, every page of
every user range is translated and checked for user access, leaf level,
writability where required, canonical range shape, overflow, and allocatable
physical backing. Process termination cancels owned work and invalidates its
token.

Operations cover monotonic time, bounded random bytes, DNS, TCP lifecycle and
I/O, poll, cancel, HTTP-to-memory, and HTTP-to-file. The
`network-http-length` scenario constructs a real private process address space,
dispatches HTTP-to-FAT32 through this boundary, authenticates the response,
invalidates the terminated token, and proves complete page/frame teardown.

## Terminal use

```text
network
dhcp
ip 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3
arp
ping 10.0.2.2 1
resolve sapote.test
http http://sapote.test/welcome.txt NETCAP.TXT
netstat
```

The `http` command writes only to the Data volume. `netstat` reports bounded
resource use, RX/TX counts, accepted Ethernet/IPv4/UDP traffic, malformed
packets, and IPv4 checksum failures.

## Entropy

`random.c` mixes RDSEED and RDRAND when available with calibrated timing and
monotonic state. Boot explicitly records `strong`, `hardware`, or `degraded`.
The API never claims cryptographic strength when only the degraded source is
available. DHCP/DNS/TCP identifiers still avoid fixed constants, but HTTPS and
other cryptographic protocols remain prohibited until the TLS prerequisites are
met.

## Deterministic evidence

`tools/network_fixture.py` is an offline unicast Ethernet peer with deterministic
DHCP, ARP, ICMP, UDP, DNS, TCP, and HTTP behavior. Its negative modes cover
silence/timeouts, NAK, NXDOMAIN, truncation, CNAME, bad checksum, ARP conflict,
TCP reset/retransmission, HTTP chunking/redirect/truncation/malformed framing,
redirect loops, and malformed floods. It writes classic PCAP with deterministic
packet timestamps.

`tools/network_packet_audit.py` independently reconstructs the captured
Ethernet frames and requires traffic in both directions plus ARP, IPv4, ICMP,
UDP, DHCP, DNS, TCP, and HTTP. The production proof is false if any layer is
missing. `tools/run_network_scenario.py` owns fixture/QEMU lifecycle, isolated
ports, per-test storage copies, link-down QMP control, stable exit codes, serial
markers, and packet audit.

## Measured reference run

These are diagnostic measurements from the 30-byte offline fixture under QEMU
TCG on the v2.1.0 development host, not general throughput claims:

| Path | HTTP elapsed | FAT32 sync | payload rate | polling CPU | interrupt CPU | drops |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| chunked | 44.53 ms | 16.10 ms | 673 B/s | 2.34 ms | 0.087 ms | 0 |
| one redirect | 46.62 ms | 16.06 ms | 643 B/s | 4.42 ms | 0.136 ms | 0 |

The 22-second evidence interaction completed DHCP, ping, DNS, HTTP streaming,
FAT32 synchronization, screen updates, keyboard/pointer input, and `netstat`
without packet drops. Larger-body and multi-connection performance remains
future work.


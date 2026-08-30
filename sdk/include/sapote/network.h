/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_NETWORK_H
#define SAPOTE_USER_NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <sapote/abi.h>

long sapote_dns_resolve(const char *hostname, uint64_t deadline_ns);
long sapote_stream_open(void);
long sapote_stream_connect(sapote_handle_t stream,
    const struct sapote_ipv4_endpoint *endpoint, uint64_t deadline_ns);
long sapote_stream_read(sapote_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns);
long sapote_stream_write(sapote_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns);
long sapote_stream_shutdown(sapote_handle_t stream, uint32_t flags,
    uint64_t deadline_ns);
long sapote_datagram_open(void);
long sapote_datagram_bind(sapote_handle_t datagram, uint16_t port);
long sapote_datagram_send(sapote_handle_t datagram,
    const struct sapote_ipv4_endpoint *destination, const void *buffer,
    size_t length, uint64_t deadline_ns);
long sapote_datagram_receive(sapote_handle_t datagram,
    struct sapote_ipv4_endpoint *source, void *buffer, size_t length,
    uint64_t deadline_ns);
long sapote_network_address(sapote_handle_t handle, int peer,
    struct sapote_ipv4_endpoint *endpoint);
long sapote_network_cancel(sapote_handle_t handle);

#endif

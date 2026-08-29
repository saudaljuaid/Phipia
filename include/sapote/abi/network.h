/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_NETWORK_H
#define SAPOTE_ABI_NETWORK_H

#include <sapote/abi/base.h>

struct sapote_ipv4_endpoint {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
} __attribute__((packed));

struct sapote_network_io {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t buffer;
    uint64_t deadline_ns;
    struct sapote_ipv4_endpoint endpoint;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

enum sapote_stream_shutdown {
    SAPOTE_SHUTDOWN_WRITE = UINT32_C(1) << 0,
    SAPOTE_SHUTDOWN_READ = UINT32_C(1) << 1
};

_Static_assert(sizeof(struct sapote_ipv4_endpoint) == 8U,
    "Sapote IPv4 endpoint ABI changed");
_Static_assert(sizeof(struct sapote_network_io) == 48U,
    "Sapote network-I/O ABI changed");

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_MEMORY_H
#define SAPOTE_ABI_MEMORY_H

#include <sapote/abi/base.h>

enum sapote_memory_flags {
    SAPOTE_MEMORY_READ = UINT32_C(1) << 0,
    SAPOTE_MEMORY_WRITE = UINT32_C(1) << 1,
    SAPOTE_MEMORY_GUARD_BEFORE = UINT32_C(1) << 2,
    SAPOTE_MEMORY_GUARD_AFTER = UINT32_C(1) << 3
};

#define SAPOTE_MEMORY_FLAGS_V1 (SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE | \
    SAPOTE_MEMORY_GUARD_BEFORE | SAPOTE_MEMORY_GUARD_AFTER)

struct sapote_memory_map_request {
    uint32_t size;
    uint32_t version;
    uint64_t length;
    uint64_t address_hint;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct sapote_memory_map_response {
    uint32_t size;
    uint32_t version;
    uint64_t address;
    uint64_t length;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_memory_map_request) == 32U,
    "Sapote memory-map request ABI changed");
_Static_assert(sizeof(struct sapote_memory_map_response) == 24U,
    "Sapote memory-map response ABI changed");

#endif

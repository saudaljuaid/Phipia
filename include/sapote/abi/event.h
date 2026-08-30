/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_EVENT_H
#define SAPOTE_ABI_EVENT_H

#include <sapote/abi/base.h>

#define SAPOTE_WAIT_MAX 8U

enum sapote_wait_interest {
    SAPOTE_WAIT_READABLE = UINT32_C(1) << 0,
    SAPOTE_WAIT_WRITABLE = UINT32_C(1) << 1,
    SAPOTE_WAIT_ACCEPTABLE = UINT32_C(1) << 2,
    SAPOTE_WAIT_SIGNALED = UINT32_C(1) << 3,
    SAPOTE_WAIT_CLOSED = UINT32_C(1) << 4
};

#define SAPOTE_WAIT_INTERESTS_V1 ((UINT32_C(1) << 5) - UINT32_C(1))

struct sapote_wait_item {
    sapote_handle_t handle;
    uint32_t interests;
    uint32_t ready;
} __attribute__((packed));

struct sapote_wait_request {
    uint32_t size;
    uint32_t version;
    uint64_t items;
    uint64_t deadline_ns;
    uint32_t count;
    uint32_t flags;
} __attribute__((packed));

struct sapote_timer_set_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t deadline_ns;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_wait_item) == 16U,
    "Sapote wait-item ABI changed");
_Static_assert(sizeof(struct sapote_wait_request) == 32U,
    "Sapote wait-request ABI changed");
_Static_assert(sizeof(struct sapote_timer_set_request) == 32U,
    "Sapote timer-set ABI changed");

#endif

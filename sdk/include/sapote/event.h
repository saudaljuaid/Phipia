/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_EVENT_H
#define SAPOTE_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/abi.h>

long sapote_wait(struct sapote_wait_item *items, size_t count,
    uint64_t deadline_ns);
long sapote_timer_create(void);
long sapote_timer_set(sapote_handle_t timer, uint64_t deadline_ns);
long sapote_cancel(sapote_handle_t handle);

#endif

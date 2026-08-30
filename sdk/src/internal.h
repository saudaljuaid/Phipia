/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_RUNTIME_INTERNAL_H
#define SAPOTE_RUNTIME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/runtime.h>

struct sapote_runtime_path {
    uint16_t volume;
    const char *text;
    size_t length;
};

int sapote_runtime_path(const char *input, struct sapote_runtime_path *result);
void sapote_runtime_lock(volatile uint32_t *lock);
void sapote_runtime_unlock(volatile uint32_t *lock);
size_t sapote_allocation_size(const void *pointer);

#endif

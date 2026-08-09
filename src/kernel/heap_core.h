/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_HEAP_CORE_H
#define ZENITH_HEAP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/heap.h>

#define HEAP_CORE_INDEX_NONE UINT32_MAX

enum heap_core_status {
    HEAP_CORE_STATUS_OK = 0,
    HEAP_CORE_STATUS_NULL_ARGUMENT,
    HEAP_CORE_STATUS_ALREADY_INITIALIZED,
    HEAP_CORE_STATUS_NOT_INITIALIZED,
    HEAP_CORE_STATUS_BAD_CAPACITY,
    HEAP_CORE_STATUS_ZERO_SIZE,
    HEAP_CORE_STATUS_INVALID_ALIGNMENT,
    HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW,
    HEAP_CORE_STATUS_OUT_OF_SPACE,
    HEAP_CORE_STATUS_DESCRIPTOR_LIMIT,
    HEAP_CORE_STATUS_POINTER_OUTSIDE,
    HEAP_CORE_STATUS_MISALIGNED_POINTER,
    HEAP_CORE_STATUS_INTERIOR_POINTER,
    HEAP_CORE_STATUS_INVALID_POINTER,
    HEAP_CORE_STATUS_DOUBLE_FREE,
    HEAP_CORE_STATUS_CORRUPTED_STATE,
    HEAP_CORE_STATUS_STATS_INVALID
};

enum heap_core_descriptor_state {
    HEAP_CORE_DESCRIPTOR_UNUSED = 0,
    HEAP_CORE_DESCRIPTOR_FREE,
    HEAP_CORE_DESCRIPTOR_LIVE
};

struct heap_core_descriptor {
    uint64_t offset;
    uint64_t extent_size;
    uint64_t requested_size;
    uint64_t alignment;
    uint64_t integrity;
    uint32_t previous;
    uint32_t next;
    uint32_t state;
    uint32_t reserved;
};

struct heap_core_state {
    uint64_t magic;
    uint64_t inverse_magic;
    uint64_t capacity;
    uint64_t committed;
    uint64_t live_allocations;
    uint64_t live_requested_bytes;
    uint64_t live_extent_bytes;
    uint32_t head;
    uint32_t descriptor_limit;
    struct heap_core_descriptor descriptors[HEAP_DESCRIPTOR_LIMIT];
};

struct heap_core_stats {
    uint64_t capacity;
    uint64_t committed;
    uint64_t live_allocations;
    uint64_t live_requested_bytes;
    uint64_t live_extent_bytes;
    uint64_t reusable_bytes;
    uint64_t largest_reusable_block;
    uint32_t descriptors_in_use;
    uint32_t descriptor_limit;
};

void heap_core_clear(struct heap_core_state *state);
void heap_core_copy(
    struct heap_core_state *destination,
    const struct heap_core_state *source
);
enum heap_core_status heap_core_initialize(
    struct heap_core_state *state,
    uint64_t capacity
);
enum heap_core_status heap_core_validate(
    const struct heap_core_state *state
);
enum heap_core_status heap_core_grow(
    struct heap_core_state *state,
    uint64_t additional_bytes
);
enum heap_core_status heap_core_growth_needed(
    const struct heap_core_state *state,
    uint64_t byte_size,
    uint64_t alignment,
    uint64_t *additional_bytes
);
enum heap_core_status heap_core_allocate(
    struct heap_core_state *state,
    uint64_t byte_size,
    uint64_t alignment,
    uint64_t *offset
);
enum heap_core_status heap_core_deallocate(
    struct heap_core_state *state,
    uint64_t offset
);
enum heap_core_status heap_core_get_stats(
    const struct heap_core_state *state,
    struct heap_core_stats *stats
);
const char *heap_core_status_string(enum heap_core_status status);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_HEAP_H
#define ZENITH_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_MINIMUM_ALIGNMENT ((size_t)16U)
#define HEAP_MAXIMUM_ALIGNMENT ((size_t)4096U)
#define HEAP_DESCRIPTOR_LIMIT ((size_t)512U)

enum heap_status {
    HEAP_STATUS_OK = 0,
    HEAP_STATUS_NULL_ARGUMENT,
    HEAP_STATUS_ALREADY_INITIALIZED,
    HEAP_STATUS_NOT_INITIALIZED,
    HEAP_STATUS_FORBIDDEN_CONTEXT,
    HEAP_STATUS_ZERO_SIZE,
    HEAP_STATUS_INVALID_ALIGNMENT,
    HEAP_STATUS_ARITHMETIC_OVERFLOW,
    HEAP_STATUS_WINDOW_EXHAUSTED,
    HEAP_STATUS_DESCRIPTOR_LIMIT,
    HEAP_STATUS_OUT_OF_MEMORY,
    HEAP_STATUS_FRAME_FAILURE,
    HEAP_STATUS_MAPPING_FAILURE,
    HEAP_STATUS_ROLLBACK_FAILURE,
    HEAP_STATUS_POINTER_OUTSIDE_HEAP,
    HEAP_STATUS_MISALIGNED_POINTER,
    HEAP_STATUS_INTERIOR_POINTER,
    HEAP_STATUS_INVALID_POINTER,
    HEAP_STATUS_DOUBLE_FREE,
    HEAP_STATUS_CORRUPTED_STATE,
    HEAP_STATUS_STATS_INVALID,
    HEAP_STATUS_VALIDATION_FAILURE,
    HEAP_STATUS_TEST_FAILURE
};

struct heap_stats {
    size_t capacity_bytes;
    size_t committed_bytes;
    size_t mapped_pages;
    size_t live_allocations;
    size_t live_requested_bytes;
    size_t live_extent_bytes;
    size_t reusable_bytes;
    size_t largest_reusable_block;
    size_t descriptors_in_use;
    size_t descriptor_limit;
    size_t failed_allocations;
};

bool heap_self_test(void);
enum heap_status heap_initialize(void);
enum heap_status heap_allocate(
    size_t byte_size,
    size_t alignment,
    void **allocation
);
enum heap_status heap_deallocate(void *allocation);
enum heap_status heap_validate(void);
enum heap_status heap_get_stats(struct heap_stats *stats);
const char *heap_status_string(enum heap_status status);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_MEMORY_H
#define ZENITH_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>

#define ZENITH_PAGE_SIZE UINT64_C(4096)
#define ZENITH_LOW_MEMORY_RESERVATION UINT64_C(0x100000)

enum frame_status {
    FRAME_STATUS_OK = 0,
    FRAME_STATUS_NULL_ARGUMENT,
    FRAME_STATUS_NOT_INITIALIZED,
    FRAME_STATUS_BAD_MEMORY_MAP,
    FRAME_STATUS_RANGE_OVERFLOW,
    FRAME_STATUS_RANGE_OUTSIDE_LIMIT,
    FRAME_STATUS_OUT_OF_MEMORY,
    FRAME_STATUS_UNALIGNED_ADDRESS,
    FRAME_STATUS_FRAME_NOT_ALLOCATABLE,
    FRAME_STATUS_FRAME_IN_USE,
    FRAME_STATUS_DOUBLE_FREE,
    FRAME_STATUS_INVALID_OWNER,
    FRAME_STATUS_OWNER_MISMATCH,
    FRAME_STATUS_VALIDATION_FAILURE,
    FRAME_STATUS_TEST_FAILURE
};

enum frame_owner {
    FRAME_OWNER_NONE = 0,
    FRAME_OWNER_GENERIC,
    FRAME_OWNER_KERNEL_HEAP
};

struct frame_allocator_stats {
    size_t addressable_frames;
    size_t allocatable_frames;
    size_t free_frames;
    size_t allocated_frames;
    size_t generic_allocated_frames;
    size_t heap_allocated_frames;
    size_t reserved_frames;
    uint64_t highest_allocatable_address;
};

enum frame_status frame_allocator_initialize(const struct boot_context *context);
enum frame_status frame_allocate(uintptr_t *physical_address);
enum frame_status frame_allocate_owned(
    enum frame_owner owner,
    uintptr_t *physical_address
);
enum frame_status frame_release(uintptr_t physical_address);
enum frame_status frame_release_owned(
    uintptr_t physical_address,
    enum frame_owner owner
);
enum frame_status frame_query_allocation(
    uintptr_t physical_address,
    bool *allocated
);
enum frame_status frame_query_owner(
    uintptr_t physical_address,
    enum frame_owner *owner
);
enum frame_status frame_reserve_range(uint64_t base_address, uint64_t length);
enum frame_status frame_allocator_validate(void);
struct frame_allocator_stats frame_allocator_get_stats(void);
const char *frame_status_string(enum frame_status status);

#endif

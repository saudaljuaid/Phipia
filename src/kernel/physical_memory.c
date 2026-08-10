/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>
#include <zenith/memory.h>
#include <zenith/test.h>

#define FRAME_COUNT ((size_t)(ZENITH_EARLY_PHYSICAL_LIMIT / ZENITH_PAGE_SIZE))
#define BITMAP_BYTE_COUNT ((FRAME_COUNT + 7U) / 8U)

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static uint8_t eligible_bitmap[BITMAP_BYTE_COUNT];
static uint8_t used_bitmap[BITMAP_BYTE_COUNT];
static uint8_t heap_owner_bitmap[BITMAP_BYTE_COUNT];
static uint8_t task_stack_owner_bitmap[BITMAP_BYTE_COUNT];
static struct frame_allocator_stats allocator_stats;
static size_t next_search_index;
static size_t test_allocation_successes_remaining;
static bool test_allocation_failure_enabled;
static bool test_allocation_reports_out_of_memory;
static bool allocator_initialized;

static bool bitmap_get(const uint8_t *bitmap, size_t frame_index)
{
    const uint8_t mask = (uint8_t)(1U << (frame_index % 8U));

    return (bitmap[frame_index / 8U] & mask) != 0U;
}

static void bitmap_set(uint8_t *bitmap, size_t frame_index, bool value)
{
    const uint8_t mask = (uint8_t)(1U << (frame_index % 8U));
    uint8_t *byte = &bitmap[frame_index / 8U];

    if (value) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

static void bitmap_fill(uint8_t *bitmap, uint8_t value)
{
    for (size_t index = 0; index < BITMAP_BYTE_COUNT; ++index) {
        bitmap[index] = value;
    }
}

static bool checked_range_end(uint64_t base, uint64_t length, uint64_t *end)
{
    if (length > UINT64_MAX - base) {
        return false;
    }

    *end = base + length;
    return true;
}

static enum frame_status available_frame_bounds(
    uint64_t base,
    uint64_t length,
    size_t *first_frame,
    size_t *past_last_frame
)
{
    const uint64_t page_mask = ZENITH_PAGE_SIZE - 1U;
    uint64_t end;
    uint64_t clipped_end;
    uint64_t aligned_base;

    if (!checked_range_end(base, length, &end)) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    if (length == 0U || base >= ZENITH_EARLY_PHYSICAL_LIMIT) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    clipped_end = end < ZENITH_EARLY_PHYSICAL_LIMIT
        ? end
        : ZENITH_EARLY_PHYSICAL_LIMIT;
    aligned_base = (base + page_mask) & ~page_mask;

    if (aligned_base >= clipped_end) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    *first_frame = (size_t)(aligned_base / ZENITH_PAGE_SIZE);
    *past_last_frame = (size_t)((clipped_end & ~page_mask) / ZENITH_PAGE_SIZE);
    return FRAME_STATUS_OK;
}

static enum frame_status covering_frame_bounds(
    uint64_t base,
    uint64_t length,
    size_t *first_frame,
    size_t *past_last_frame
)
{
    uint64_t end;
    uint64_t clipped_end;

    if (!checked_range_end(base, length, &end)) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    if (length == 0U) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    if (base >= ZENITH_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    clipped_end = end < ZENITH_EARLY_PHYSICAL_LIMIT
        ? end
        : ZENITH_EARLY_PHYSICAL_LIMIT;
    *first_frame = (size_t)(base / ZENITH_PAGE_SIZE);
    *past_last_frame = (size_t)(clipped_end / ZENITH_PAGE_SIZE);

    if ((clipped_end & (ZENITH_PAGE_SIZE - 1U)) != 0U) {
        ++*past_last_frame;
    }

    return FRAME_STATUS_OK;
}

static void mark_available_frames(size_t first_frame, size_t past_last_frame)
{
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        bitmap_set(eligible_bitmap, frame, true);
        bitmap_set(used_bitmap, frame, false);
        bitmap_set(heap_owner_bitmap, frame, false);
        bitmap_set(task_stack_owner_bitmap, frame, false);
    }
}

static void mark_reserved_frames(size_t first_frame, size_t past_last_frame)
{
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        bitmap_set(eligible_bitmap, frame, false);
        bitmap_set(used_bitmap, frame, true);
        bitmap_set(heap_owner_bitmap, frame, false);
        bitmap_set(task_stack_owner_bitmap, frame, false);
    }
}

static bool owner_is_allocatable(enum frame_owner owner)
{
    return owner == FRAME_OWNER_GENERIC ||
        owner == FRAME_OWNER_KERNEL_HEAP ||
        owner == FRAME_OWNER_TASK_STACK;
}

static bool allocator_stats_are_locally_valid(void)
{
    return allocator_stats.addressable_frames == FRAME_COUNT &&
        allocator_stats.allocatable_frames <= FRAME_COUNT &&
        allocator_stats.reserved_frames <= FRAME_COUNT &&
        allocator_stats.allocatable_frames + allocator_stats.reserved_frames ==
            FRAME_COUNT &&
        allocator_stats.free_frames <= allocator_stats.allocatable_frames &&
        allocator_stats.allocated_frames <= allocator_stats.allocatable_frames &&
        allocator_stats.free_frames + allocator_stats.allocated_frames ==
            allocator_stats.allocatable_frames &&
        allocator_stats.generic_allocated_frames <=
            allocator_stats.allocated_frames &&
        allocator_stats.heap_allocated_frames <=
            allocator_stats.allocated_frames &&
        allocator_stats.task_stack_allocated_frames <=
            allocator_stats.allocated_frames &&
        allocator_stats.generic_allocated_frames +
                allocator_stats.heap_allocated_frames +
                allocator_stats.task_stack_allocated_frames ==
            allocator_stats.allocated_frames &&
        allocator_stats.highest_allocatable_address <=
            ZENITH_EARLY_PHYSICAL_LIMIT &&
        allocator_stats.highest_allocatable_address % ZENITH_PAGE_SIZE == 0U &&
        ((allocator_stats.allocatable_frames == 0U) ==
            (allocator_stats.highest_allocatable_address == 0U));
}

static enum frame_status reserve_internal(uint64_t base, uint64_t length)
{
    size_t first_frame;
    size_t past_last_frame;
    enum frame_status status = covering_frame_bounds(
        base,
        length,
        &first_frame,
        &past_last_frame
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    mark_reserved_frames(first_frame, past_last_frame);
    return FRAME_STATUS_OK;
}

static void recompute_stats(void)
{
    struct frame_allocator_stats stats = {
        .addressable_frames = FRAME_COUNT,
        .allocatable_frames = 0,
        .free_frames = 0,
        .allocated_frames = 0,
        .generic_allocated_frames = 0,
        .heap_allocated_frames = 0,
        .task_stack_allocated_frames = 0,
        .reserved_frames = 0,
        .highest_allocatable_address = 0
    };

    for (size_t frame = 0; frame < FRAME_COUNT; ++frame) {
        if (!bitmap_get(eligible_bitmap, frame)) {
            ++stats.reserved_frames;
            continue;
        }

        ++stats.allocatable_frames;
        stats.highest_allocatable_address =
            ((uint64_t)frame + 1U) * ZENITH_PAGE_SIZE;

        if (bitmap_get(used_bitmap, frame)) {
            ++stats.allocated_frames;

            if (bitmap_get(heap_owner_bitmap, frame)) {
                ++stats.heap_allocated_frames;
            } else if (bitmap_get(task_stack_owner_bitmap, frame)) {
                ++stats.task_stack_allocated_frames;
            } else {
                ++stats.generic_allocated_frames;
            }
        } else {
            ++stats.free_frames;
        }
    }

    allocator_stats = stats;
}

static enum frame_status apply_memory_map(const struct boot_context *context)
{
    for (size_t pass = 0; pass < 2U; ++pass) {
        for (size_t index = 0; index < context->memory_map_entry_count; ++index) {
            struct boot_memory_region region;
            size_t first_frame;
            size_t past_last_frame;
            enum frame_status status;
            bool available;

            if (!boot_memory_region_at(context, index, &region)) {
                return FRAME_STATUS_BAD_MEMORY_MAP;
            }

            available = region.type == MULTIBOOT2_MEMORY_AVAILABLE;

            if ((pass == 0U) != available) {
                continue;
            }

            if (available) {
                status = available_frame_bounds(
                    region.base_address,
                    region.length,
                    &first_frame,
                    &past_last_frame
                );
            } else {
                status = covering_frame_bounds(
                    region.base_address,
                    region.length,
                    &first_frame,
                    &past_last_frame
                );

                if (status == FRAME_STATUS_RANGE_OUTSIDE_LIMIT) {
                    continue;
                }
            }

            if (status != FRAME_STATUS_OK) {
                return status;
            }

            if (available) {
                mark_available_frames(first_frame, past_last_frame);
            } else {
                mark_reserved_frames(first_frame, past_last_frame);
            }
        }
    }

    return FRAME_STATUS_OK;
}

enum frame_status frame_allocator_initialize(const struct boot_context *context)
{
    enum frame_status status;
    uint64_t kernel_start;
    uint64_t kernel_end;

    if (context == NULL || context->memory_map == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    allocator_initialized = false;
    next_search_index = 0;
    test_allocation_successes_remaining = 0U;
    test_allocation_failure_enabled = false;
    test_allocation_reports_out_of_memory = false;
    bitmap_fill(eligible_bitmap, 0U);
    bitmap_fill(used_bitmap, UINT8_MAX);
    bitmap_fill(heap_owner_bitmap, 0U);
    bitmap_fill(task_stack_owner_bitmap, 0U);

    status = apply_memory_map(context);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    status = reserve_internal(0U, ZENITH_LOW_MEMORY_RESERVATION);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    kernel_end = (uint64_t)(uintptr_t)__kernel_end;

    if (kernel_end < kernel_start) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    status = reserve_internal(kernel_start, kernel_end - kernel_start);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    status = reserve_internal(
        context->information_start,
        context->information_end - context->information_start
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    recompute_stats();

    if (allocator_stats.free_frames == 0U) {
        return FRAME_STATUS_OUT_OF_MEMORY;
    }

    next_search_index = (size_t)(ZENITH_LOW_MEMORY_RESERVATION / ZENITH_PAGE_SIZE);
    allocator_initialized = true;
    return FRAME_STATUS_OK;
}

enum frame_status frame_allocate(uintptr_t *physical_address)
{
    return frame_allocate_owned(FRAME_OWNER_GENERIC, physical_address);
}

enum frame_status frame_allocate_owned(
    enum frame_owner owner,
    uintptr_t *physical_address
)
{
    if (physical_address == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    if (!owner_is_allocatable(owner)) {
        return FRAME_STATUS_INVALID_OWNER;
    }

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (allocator_stats.free_frames == 0U) {
        return FRAME_STATUS_OUT_OF_MEMORY;
    }

    if (test_allocation_failure_enabled &&
        test_allocation_successes_remaining == 0U) {
        test_allocation_failure_enabled = false;
        return test_allocation_reports_out_of_memory
            ? FRAME_STATUS_OUT_OF_MEMORY
            : FRAME_STATUS_TEST_FAILURE;
    }

    for (size_t step = 0; step < FRAME_COUNT; ++step) {
        size_t frame = next_search_index + step;

        if (frame >= FRAME_COUNT) {
            frame -= FRAME_COUNT;
        }

        if (bitmap_get(eligible_bitmap, frame) && !bitmap_get(used_bitmap, frame)) {
            bitmap_set(used_bitmap, frame, true);
            bitmap_set(
                heap_owner_bitmap,
                frame,
                owner == FRAME_OWNER_KERNEL_HEAP
            );
            bitmap_set(
                task_stack_owner_bitmap,
                frame,
                owner == FRAME_OWNER_TASK_STACK
            );
            --allocator_stats.free_frames;
            ++allocator_stats.allocated_frames;

            if (owner == FRAME_OWNER_KERNEL_HEAP) {
                ++allocator_stats.heap_allocated_frames;
            } else if (owner == FRAME_OWNER_TASK_STACK) {
                ++allocator_stats.task_stack_allocated_frames;
            } else {
                ++allocator_stats.generic_allocated_frames;
            }
            next_search_index = frame + 1U;

            if (next_search_index == FRAME_COUNT) {
                next_search_index = 0;
            }

            *physical_address = (uintptr_t)((uint64_t)frame * ZENITH_PAGE_SIZE);

            if (test_allocation_failure_enabled) {
                --test_allocation_successes_remaining;
            }

            return FRAME_STATUS_OK;
        }
    }

    return FRAME_STATUS_OUT_OF_MEMORY;
}

enum frame_status frame_release(uintptr_t physical_address)
{
    return frame_release_owned(physical_address, FRAME_OWNER_GENERIC);
}

enum frame_status frame_release_owned(
    uintptr_t physical_address,
    enum frame_owner owner
)
{
    size_t frame;

    if (!owner_is_allocatable(owner)) {
        return FRAME_STATUS_INVALID_OWNER;
    }

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (((uint64_t)physical_address & (ZENITH_PAGE_SIZE - 1U)) != 0U) {
        return FRAME_STATUS_UNALIGNED_ADDRESS;
    }

    if ((uint64_t)physical_address >= ZENITH_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    frame = (size_t)((uint64_t)physical_address / ZENITH_PAGE_SIZE);

    if (!bitmap_get(eligible_bitmap, frame)) {
        return FRAME_STATUS_FRAME_NOT_ALLOCATABLE;
    }

    if (!bitmap_get(used_bitmap, frame)) {
        return FRAME_STATUS_DOUBLE_FREE;
    }

    if (bitmap_get(heap_owner_bitmap, frame) !=
            (owner == FRAME_OWNER_KERNEL_HEAP) ||
        bitmap_get(task_stack_owner_bitmap, frame) !=
            (owner == FRAME_OWNER_TASK_STACK)) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    bitmap_set(used_bitmap, frame, false);
    bitmap_set(heap_owner_bitmap, frame, false);
    bitmap_set(task_stack_owner_bitmap, frame, false);
    ++allocator_stats.free_frames;
    --allocator_stats.allocated_frames;

    if (owner == FRAME_OWNER_KERNEL_HEAP) {
        --allocator_stats.heap_allocated_frames;
    } else if (owner == FRAME_OWNER_TASK_STACK) {
        --allocator_stats.task_stack_allocated_frames;
    } else {
        --allocator_stats.generic_allocated_frames;
    }

    if (frame < next_search_index) {
        next_search_index = frame;
    }

    return FRAME_STATUS_OK;
}

enum frame_status frame_query_allocation(
    uintptr_t physical_address,
    bool *allocated
)
{
    size_t frame;

    if (allocated == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    *allocated = false;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (((uint64_t)physical_address & (ZENITH_PAGE_SIZE - 1U)) != 0U) {
        return FRAME_STATUS_UNALIGNED_ADDRESS;
    }

    if ((uint64_t)physical_address >= ZENITH_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    frame = (size_t)((uint64_t)physical_address / ZENITH_PAGE_SIZE);

    if (!bitmap_get(eligible_bitmap, frame)) {
        return FRAME_STATUS_FRAME_NOT_ALLOCATABLE;
    }

    *allocated = bitmap_get(used_bitmap, frame);
    return FRAME_STATUS_OK;
}

enum frame_status frame_query_owner(
    uintptr_t physical_address,
    enum frame_owner *owner
)
{
    size_t frame;

    if (owner == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    *owner = FRAME_OWNER_NONE;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (((uint64_t)physical_address & (ZENITH_PAGE_SIZE - 1U)) != 0U) {
        return FRAME_STATUS_UNALIGNED_ADDRESS;
    }

    if ((uint64_t)physical_address >= ZENITH_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    frame = (size_t)((uint64_t)physical_address / ZENITH_PAGE_SIZE);

    if (!bitmap_get(eligible_bitmap, frame)) {
        return FRAME_STATUS_FRAME_NOT_ALLOCATABLE;
    }

    if (bitmap_get(used_bitmap, frame)) {
        if (bitmap_get(heap_owner_bitmap, frame)) {
            *owner = FRAME_OWNER_KERNEL_HEAP;
        } else if (bitmap_get(task_stack_owner_bitmap, frame)) {
            *owner = FRAME_OWNER_TASK_STACK;
        } else {
            *owner = FRAME_OWNER_GENERIC;
        }
    }

    return FRAME_STATUS_OK;
}

enum frame_status frame_reserve_range(uint64_t base_address, uint64_t length)
{
    size_t first_frame;
    size_t past_last_frame;
    enum frame_status status;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    status = covering_frame_bounds(
        base_address,
        length,
        &first_frame,
        &past_last_frame
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        if (bitmap_get(eligible_bitmap, frame) && bitmap_get(used_bitmap, frame)) {
            return FRAME_STATUS_FRAME_IN_USE;
        }
    }

    mark_reserved_frames(first_frame, past_last_frame);
    recompute_stats();
    return FRAME_STATUS_OK;
}

enum frame_status frame_allocator_validate(void)
{
    struct frame_allocator_stats observed = {
        .addressable_frames = FRAME_COUNT,
        .allocatable_frames = 0U,
        .free_frames = 0U,
        .allocated_frames = 0U,
        .generic_allocated_frames = 0U,
        .heap_allocated_frames = 0U,
        .task_stack_allocated_frames = 0U,
        .reserved_frames = 0U,
        .highest_allocatable_address = 0U
    };

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (next_search_index >= FRAME_COUNT) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    for (size_t frame = 0U; frame < FRAME_COUNT; ++frame) {
        const bool eligible = bitmap_get(eligible_bitmap, frame);
        const bool used = bitmap_get(used_bitmap, frame);
        const bool heap_owned = bitmap_get(heap_owner_bitmap, frame);
        const bool task_stack_owned =
            bitmap_get(task_stack_owner_bitmap, frame);

        if (!eligible) {
            if (!used || heap_owned || task_stack_owned) {
                return FRAME_STATUS_VALIDATION_FAILURE;
            }

            ++observed.reserved_frames;
            continue;
        }

        ++observed.allocatable_frames;
        observed.highest_allocatable_address =
            ((uint64_t)frame + 1U) * ZENITH_PAGE_SIZE;

        if (used) {
            if (heap_owned && task_stack_owned) {
                return FRAME_STATUS_VALIDATION_FAILURE;
            }

            ++observed.allocated_frames;

            if (heap_owned) {
                ++observed.heap_allocated_frames;
            } else if (task_stack_owned) {
                ++observed.task_stack_allocated_frames;
            } else {
                ++observed.generic_allocated_frames;
            }
        } else {
            if (heap_owned || task_stack_owned) {
                return FRAME_STATUS_VALIDATION_FAILURE;
            }

            ++observed.free_frames;
        }
    }

    if (observed.addressable_frames != allocator_stats.addressable_frames ||
        observed.allocatable_frames != allocator_stats.allocatable_frames ||
        observed.free_frames != allocator_stats.free_frames ||
        observed.allocated_frames != allocator_stats.allocated_frames ||
        observed.generic_allocated_frames !=
            allocator_stats.generic_allocated_frames ||
        observed.heap_allocated_frames !=
            allocator_stats.heap_allocated_frames ||
        observed.task_stack_allocated_frames !=
            allocator_stats.task_stack_allocated_frames ||
        observed.reserved_frames != allocator_stats.reserved_frames ||
        observed.highest_allocatable_address !=
            allocator_stats.highest_allocatable_address ||
        observed.allocatable_frames + observed.reserved_frames != FRAME_COUNT ||
        observed.free_frames + observed.allocated_frames !=
            observed.allocatable_frames ||
        observed.generic_allocated_frames + observed.heap_allocated_frames +
                observed.task_stack_allocated_frames !=
            observed.allocated_frames) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    return FRAME_STATUS_OK;
}

bool frame_test_fail_allocate_after(size_t successful_allocations)
{
    if (!allocator_initialized || test_allocation_failure_enabled) {
        return false;
    }

    test_allocation_successes_remaining = successful_allocations;
    test_allocation_failure_enabled = true;
    test_allocation_reports_out_of_memory = false;
    return true;
}

bool frame_test_report_out_of_memory_after(size_t successful_allocations)
{
    if (!allocator_initialized || test_allocation_failure_enabled) {
        return false;
    }

    test_allocation_successes_remaining = successful_allocations;
    test_allocation_failure_enabled = true;
    test_allocation_reports_out_of_memory = true;
    return true;
}

struct frame_allocator_stats frame_allocator_get_stats(void)
{
    return allocator_stats;
}

const char *frame_status_string(enum frame_status status)
{
    switch (status) {
    case FRAME_STATUS_OK:
        return "ok";
    case FRAME_STATUS_NULL_ARGUMENT:
        return "null frame allocator argument";
    case FRAME_STATUS_NOT_INITIALIZED:
        return "frame allocator is not initialized";
    case FRAME_STATUS_BAD_MEMORY_MAP:
        return "invalid frame allocator memory map";
    case FRAME_STATUS_RANGE_OVERFLOW:
        return "physical range overflows";
    case FRAME_STATUS_RANGE_OUTSIDE_LIMIT:
        return "physical range is outside the early map";
    case FRAME_STATUS_OUT_OF_MEMORY:
        return "no physical frame is available";
    case FRAME_STATUS_UNALIGNED_ADDRESS:
        return "physical frame address is unaligned";
    case FRAME_STATUS_FRAME_NOT_ALLOCATABLE:
        return "physical frame is permanently reserved";
    case FRAME_STATUS_FRAME_IN_USE:
        return "physical frame is already allocated";
    case FRAME_STATUS_DOUBLE_FREE:
        return "physical frame was released twice";
    case FRAME_STATUS_INVALID_OWNER:
        return "physical frame owner is invalid";
    case FRAME_STATUS_OWNER_MISMATCH:
        return "physical frame owner does not match the releasing consumer";
    case FRAME_STATUS_VALIDATION_FAILURE:
        return "physical-frame allocator validation failed";
    case FRAME_STATUS_TEST_FAILURE:
        return "injected physical-frame allocation failure";
    default:
        return "unknown frame allocator status";
    }
}

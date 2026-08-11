/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>
#include <zenith/memory.h>
#include <zenith/spinlock.h>
#include <zenith/test.h>

#define FRAME_COUNT ((size_t)(ZENITH_EARLY_PHYSICAL_LIMIT / ZENITH_PAGE_SIZE))
#define BITMAP_WORD_BITS ((size_t)64U)
#define BITMAP_WORD_COUNT ((FRAME_COUNT + BITMAP_WORD_BITS - 1U) / \
                           BITMAP_WORD_BITS)
#define FRAME_ALLOCATOR_LOCK_INDEX UINT32_C(4)

_Static_assert(FRAME_COUNT % BITMAP_WORD_BITS == 0U,
               "the early frame limit must fill complete bitmap words");

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static uint64_t eligible_bitmap[BITMAP_WORD_COUNT];
static uint64_t used_bitmap[BITMAP_WORD_COUNT];
static uint64_t heap_owner_bitmap[BITMAP_WORD_COUNT];
static uint64_t task_stack_owner_bitmap[BITMAP_WORD_COUNT];
static uint64_t process_page_owner_bitmap[BITMAP_WORD_COUNT];
/* Exact 32 KiB scratch: 4,096 heap frames times one 64-bit uintptr_t. */
static uintptr_t
    heap_owner_validation_frames[FRAME_ALLOCATOR_HEAP_VALIDATION_LIMIT];
/* Exact 4 KiB scratch: sixteen spaces times thirty-two process pages. */
static uintptr_t process_page_owner_validation_frames[
    FRAME_ALLOCATOR_PROCESS_PAGE_VALIDATION_LIMIT
];
static struct frame_allocator_stats allocator_stats;
static size_t next_search_index;
static size_t test_allocation_successes_remaining;
static bool test_allocation_failure_enabled;
static bool test_allocation_reports_out_of_memory;
static bool allocator_poisoned;
static bool allocator_initialized;
static uint64_t task_stack_mutation_epoch;
static uint64_t process_page_mutation_epoch;
static bool allocator_lock_registered;
static struct spinlock allocator_lock;

bool frame_test_set_process_page_mutation_epoch(uint64_t mutation_epoch);
bool frame_test_process_page_state_matches(size_t expected_owned_frames);

_Static_assert(sizeof(heap_owner_validation_frames) == 32U * 1024U,
               "heap-owner validation scratch must remain exactly 32 KiB");
_Static_assert(sizeof(process_page_owner_validation_frames) == 4U * 1024U,
               "process-page validation scratch must remain exactly 4 KiB");

static void frame_test_disarm_allocation_failure_unguarded(void)
{
    test_allocation_successes_remaining = 0U;
    test_allocation_failure_enabled = false;
    test_allocation_reports_out_of_memory = false;
}

static void frame_allocator_poison(void)
{
    allocator_poisoned = true;
    frame_test_disarm_allocation_failure_unguarded();
}

static bool task_stack_epoch_has_headroom(uint64_t required_increments)
{
    if (required_increments == 0U ||
        task_stack_mutation_epoch > UINT64_MAX - required_increments) {
        frame_allocator_poison();
        return false;
    }

    return true;
}

static bool process_page_epoch_has_headroom(uint64_t required_increments)
{
    if (required_increments == 0U ||
        process_page_mutation_epoch > UINT64_MAX - required_increments) {
        frame_allocator_poison();
        return false;
    }

    return true;
}

static bool bitmap_get(const uint64_t *bitmap, size_t frame_index)
{
    const uint64_t mask = UINT64_C(1) << (frame_index % BITMAP_WORD_BITS);

    return (bitmap[frame_index / BITMAP_WORD_BITS] & mask) != 0U;
}

static void bitmap_set(uint64_t *bitmap, size_t frame_index, bool value)
{
    const uint64_t mask = UINT64_C(1) << (frame_index % BITMAP_WORD_BITS);
    uint64_t *word = &bitmap[frame_index / BITMAP_WORD_BITS];

    if (value) {
        *word |= mask;
    } else {
        *word &= ~mask;
    }
}

static void bitmap_fill(uint64_t *bitmap, uint64_t value)
{
    for (size_t index = 0; index < BITMAP_WORD_COUNT; ++index) {
        bitmap[index] = value;
    }
}

static void frame_value_swap(uintptr_t *left, uintptr_t *right)
{
    const uintptr_t saved = *left;

    *left = *right;
    *right = saved;
}

static void frame_heap_sift_down(
    uintptr_t *frames,
    size_t count,
    size_t root
)
{
    while (root < (count >> 1U)) {
        size_t child = root * 2U + 1U;

        if (child + 1U < count && frames[child] < frames[child + 1U]) {
            ++child;
        }

        if (frames[root] >= frames[child]) {
            return;
        }

        frame_value_swap(&frames[root], &frames[child]);
        root = child;
    }
}

static void frame_heap_sort(uintptr_t *frames, size_t count)
{
    for (size_t start = count >> 1U; start > 0U; --start) {
        frame_heap_sift_down(frames, count, start - 1U);
    }

    for (size_t end = count; end > 1U; --end) {
        frame_value_swap(&frames[0], &frames[end - 1U]);
        frame_heap_sift_down(frames, end - 1U, 0U);
    }
}

static size_t bitmap_word_population(uint64_t value)
{
    value -= (value >> 1U) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) +
        ((value >> 2U) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4U)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    value += value >> 8U;
    value += value >> 16U;
    value += value >> 32U;
    return (size_t)(value & UINT64_C(0x7F));
}

static size_t bitmap_word_highest_set_bit(uint64_t value)
{
    size_t bit = 0U;

    if ((value >> 32U) != 0U) {
        value >>= 32U;
        bit += 32U;
    }
    if ((value >> 16U) != 0U) {
        value >>= 16U;
        bit += 16U;
    }
    if ((value >> 8U) != 0U) {
        value >>= 8U;
        bit += 8U;
    }
    if ((value >> 4U) != 0U) {
        value >>= 4U;
        bit += 4U;
    }
    if ((value >> 2U) != 0U) {
        value >>= 2U;
        bit += 2U;
    }
    if ((value >> 1U) != 0U) {
        ++bit;
    }
    return bit;
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
        bitmap_set(process_page_owner_bitmap, frame, false);
    }
}

static void mark_reserved_frames(size_t first_frame, size_t past_last_frame)
{
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        bitmap_set(eligible_bitmap, frame, false);
        bitmap_set(used_bitmap, frame, true);
        bitmap_set(heap_owner_bitmap, frame, false);
        bitmap_set(task_stack_owner_bitmap, frame, false);
        bitmap_set(process_page_owner_bitmap, frame, false);
    }
}

static bool owner_is_allocatable(enum frame_owner owner)
{
    return owner == FRAME_OWNER_GENERIC ||
        owner == FRAME_OWNER_KERNEL_HEAP ||
        owner == FRAME_OWNER_TASK_STACK ||
        owner == FRAME_OWNER_PROCESS_PAGE;
}

static bool allocator_stats_are_locally_valid(void)
{
    return !allocator_poisoned &&
        task_stack_mutation_epoch != 0U &&
        process_page_mutation_epoch != 0U &&
        allocator_stats.addressable_frames == FRAME_COUNT &&
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
        allocator_stats.process_page_allocated_frames <=
            allocator_stats.allocated_frames &&
        allocator_stats.generic_allocated_frames +
                allocator_stats.heap_allocated_frames +
                allocator_stats.task_stack_allocated_frames +
                allocator_stats.process_page_allocated_frames ==
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
        .process_page_allocated_frames = 0,
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
            } else if (bitmap_get(process_page_owner_bitmap, frame)) {
                ++stats.process_page_allocated_frames;
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

static enum frame_status frame_allocator_initialize_unguarded(
    const struct boot_context *context
)
{
    enum frame_status status;
    uint64_t kernel_start;
    uint64_t kernel_end;

    if (context == NULL || context->memory_map == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    if (allocator_poisoned) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (task_stack_mutation_epoch == UINT64_MAX ||
        process_page_mutation_epoch == UINT64_MAX) {
        frame_allocator_poison();
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (task_stack_mutation_epoch != 0U) {
        ++task_stack_mutation_epoch;
    }
    if (process_page_mutation_epoch != 0U) {
        ++process_page_mutation_epoch;
    }

    allocator_initialized = false;
    next_search_index = 0;
    frame_test_disarm_allocation_failure_unguarded();
    bitmap_fill(eligible_bitmap, 0U);
    bitmap_fill(used_bitmap, UINT64_MAX);
    bitmap_fill(heap_owner_bitmap, 0U);
    bitmap_fill(task_stack_owner_bitmap, 0U);
    bitmap_fill(process_page_owner_bitmap, 0U);

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
    if (task_stack_mutation_epoch == 0U) {
        task_stack_mutation_epoch = 1U;
    }
    if (process_page_mutation_epoch == 0U) {
        process_page_mutation_epoch = 1U;
    }
    allocator_initialized = true;
    return FRAME_STATUS_OK;
}

static enum frame_status frame_allocate_owned_unguarded(
    enum frame_owner owner,
    uintptr_t *physical_address
)
{
    if (physical_address == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }
    *physical_address = 0U;

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
            if (owner == FRAME_OWNER_TASK_STACK &&
                !task_stack_epoch_has_headroom(2U)) {
                return FRAME_STATUS_VALIDATION_FAILURE;
            }
            if (owner == FRAME_OWNER_PROCESS_PAGE &&
                !process_page_epoch_has_headroom(2U)) {
                return FRAME_STATUS_VALIDATION_FAILURE;
            }

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
            bitmap_set(
                process_page_owner_bitmap,
                frame,
                owner == FRAME_OWNER_PROCESS_PAGE
            );
            --allocator_stats.free_frames;
            ++allocator_stats.allocated_frames;

            if (owner == FRAME_OWNER_KERNEL_HEAP) {
                ++allocator_stats.heap_allocated_frames;
            } else if (owner == FRAME_OWNER_TASK_STACK) {
                ++allocator_stats.task_stack_allocated_frames;
                ++task_stack_mutation_epoch;
            } else if (owner == FRAME_OWNER_PROCESS_PAGE) {
                ++allocator_stats.process_page_allocated_frames;
                ++process_page_mutation_epoch;
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

static enum frame_status frame_allocate_unguarded(uintptr_t *physical_address)
{
    return frame_allocate_owned_unguarded(
        FRAME_OWNER_GENERIC,
        physical_address
    );
}

static enum frame_status frame_release_owned_unguarded(
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

    if (allocator_poisoned) {
        return FRAME_STATUS_VALIDATION_FAILURE;
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
            (owner == FRAME_OWNER_TASK_STACK) ||
        bitmap_get(process_page_owner_bitmap, frame) !=
            (owner == FRAME_OWNER_PROCESS_PAGE)) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (owner == FRAME_OWNER_TASK_STACK &&
        !task_stack_epoch_has_headroom(1U)) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (owner == FRAME_OWNER_PROCESS_PAGE &&
        !process_page_epoch_has_headroom(1U)) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    bitmap_set(used_bitmap, frame, false);
    bitmap_set(heap_owner_bitmap, frame, false);
    bitmap_set(task_stack_owner_bitmap, frame, false);
    bitmap_set(process_page_owner_bitmap, frame, false);
    ++allocator_stats.free_frames;
    --allocator_stats.allocated_frames;

    if (owner == FRAME_OWNER_KERNEL_HEAP) {
        --allocator_stats.heap_allocated_frames;
    } else if (owner == FRAME_OWNER_TASK_STACK) {
        --allocator_stats.task_stack_allocated_frames;
        ++task_stack_mutation_epoch;
    } else if (owner == FRAME_OWNER_PROCESS_PAGE) {
        --allocator_stats.process_page_allocated_frames;
        ++process_page_mutation_epoch;
    } else {
        --allocator_stats.generic_allocated_frames;
    }

    if (frame < next_search_index) {
        next_search_index = frame;
    }

    return FRAME_STATUS_OK;
}

static enum frame_status frame_release_unguarded(uintptr_t physical_address)
{
    return frame_release_owned_unguarded(
        physical_address,
        FRAME_OWNER_GENERIC
    );
}

static enum frame_status frame_query_allocation_unguarded(
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

    if (allocator_poisoned) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (task_stack_mutation_epoch == 0U ||
        process_page_mutation_epoch == 0U) {
        return FRAME_STATUS_VALIDATION_FAILURE;
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

static enum frame_status frame_query_owner_unguarded(
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

    if (allocator_poisoned) {
        return FRAME_STATUS_VALIDATION_FAILURE;
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
        } else if (bitmap_get(process_page_owner_bitmap, frame)) {
            *owner = FRAME_OWNER_PROCESS_PAGE;
        } else {
            *owner = FRAME_OWNER_GENERIC;
        }
    }

    return FRAME_STATUS_OK;
}

static enum frame_status frame_reserve_range_unguarded(
    uint64_t base_address,
    uint64_t length
)
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

static enum frame_status frame_allocator_validate_unguarded(void)
{
    struct frame_allocator_stats observed = {
        .addressable_frames = FRAME_COUNT,
        .allocatable_frames = 0U,
        .free_frames = 0U,
        .allocated_frames = 0U,
        .generic_allocated_frames = 0U,
        .heap_allocated_frames = 0U,
        .task_stack_allocated_frames = 0U,
        .process_page_allocated_frames = 0U,
        .reserved_frames = 0U,
        .highest_allocatable_address = 0U
    };
    size_t highest_eligible_word = 0U;
    uint64_t highest_eligible_value = 0U;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (allocator_poisoned) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (task_stack_mutation_epoch == 0U ||
        process_page_mutation_epoch == 0U) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    if (next_search_index >= FRAME_COUNT) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    for (size_t word_index = 0U;
         word_index < BITMAP_WORD_COUNT;
         ++word_index) {
        const uint64_t eligible = eligible_bitmap[word_index];
        const uint64_t used = used_bitmap[word_index];
        const uint64_t heap_owned = heap_owner_bitmap[word_index];
        const uint64_t task_stack_owned =
            task_stack_owner_bitmap[word_index];
        const uint64_t process_page_owned =
            process_page_owner_bitmap[word_index];
        const uint64_t owners = heap_owned | task_stack_owned |
            process_page_owned;
        const uint64_t ineligible = ~eligible;
        size_t allocatable_count;
        size_t free_count;
        size_t heap_count;
        size_t task_stack_count;
        size_t process_page_count;

        if ((ineligible & ~used) != 0U ||
            (owners & ineligible) != 0U ||
            (owners & ~used) != 0U ||
            (heap_owned & task_stack_owned) != 0U ||
            (heap_owned & process_page_owned) != 0U ||
            (task_stack_owned & process_page_owned) != 0U) {
            return FRAME_STATUS_VALIDATION_FAILURE;
        }

        allocatable_count = bitmap_word_population(eligible);
        free_count = bitmap_word_population(eligible & ~used);
        heap_count = bitmap_word_population(heap_owned);
        task_stack_count = bitmap_word_population(task_stack_owned);
        process_page_count = bitmap_word_population(process_page_owned);
        observed.allocatable_frames += allocatable_count;
        observed.reserved_frames += BITMAP_WORD_BITS - allocatable_count;
        observed.allocated_frames += allocatable_count - free_count;
        observed.free_frames += free_count;
        observed.heap_allocated_frames += heap_count;
        observed.task_stack_allocated_frames += task_stack_count;
        observed.process_page_allocated_frames += process_page_count;
        observed.generic_allocated_frames +=
            allocatable_count - free_count - heap_count - task_stack_count -
                process_page_count;

        if (eligible != 0U) {
            highest_eligible_word = word_index;
            highest_eligible_value = eligible;
        }
    }

    if (highest_eligible_value != 0U) {
        const size_t highest_frame =
            highest_eligible_word * BITMAP_WORD_BITS +
            bitmap_word_highest_set_bit(highest_eligible_value);

        observed.highest_allocatable_address =
            ((uint64_t)highest_frame + 1U) * ZENITH_PAGE_SIZE;
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
        observed.process_page_allocated_frames !=
            allocator_stats.process_page_allocated_frames ||
        observed.reserved_frames != allocator_stats.reserved_frames ||
        observed.highest_allocatable_address !=
            allocator_stats.highest_allocatable_address ||
        observed.allocatable_frames + observed.reserved_frames != FRAME_COUNT ||
        observed.free_frames + observed.allocated_frames !=
            observed.allocatable_frames ||
        observed.generic_allocated_frames + observed.heap_allocated_frames +
                observed.task_stack_allocated_frames +
                observed.process_page_allocated_frames !=
            observed.allocated_frames) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    return FRAME_STATUS_OK;
}

static enum frame_status frame_allocator_validate_heap_owners_unguarded(
    const uintptr_t *committed_frames,
    size_t committed_frame_count,
    const uintptr_t *staged_frames,
    size_t staged_frame_count
)
{
    enum frame_status status;
    size_t scratch_count = 0U;
    size_t total_frame_count;

    if ((committed_frame_count != 0U && committed_frames == NULL) ||
        (staged_frame_count != 0U && staged_frames == NULL)) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    if (committed_frame_count > FRAME_ALLOCATOR_HEAP_VALIDATION_LIMIT ||
        staged_frame_count >
            FRAME_ALLOCATOR_HEAP_VALIDATION_LIMIT - committed_frame_count) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    status = frame_allocator_validate_unguarded();
    if (status != FRAME_STATUS_OK) {
        return status;
    }

    total_frame_count = committed_frame_count + staged_frame_count;
    if (total_frame_count != allocator_stats.heap_allocated_frames) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    for (size_t index = 0U; index < committed_frame_count; ++index) {
        heap_owner_validation_frames[scratch_count] = committed_frames[index];
        ++scratch_count;
    }
    for (size_t index = 0U; index < staged_frame_count; ++index) {
        heap_owner_validation_frames[scratch_count] = staged_frames[index];
        ++scratch_count;
    }

    frame_heap_sort(heap_owner_validation_frames, scratch_count);

    for (size_t index = 0U; index < scratch_count; ++index) {
        const uintptr_t physical_address =
            heap_owner_validation_frames[index];
        size_t frame;

        if ((uint64_t)physical_address >= ZENITH_EARLY_PHYSICAL_LIMIT ||
            ((uint64_t)physical_address & (ZENITH_PAGE_SIZE - 1U)) != 0U ||
            (index != 0U &&
             heap_owner_validation_frames[index - 1U] == physical_address)) {
            status = FRAME_STATUS_OWNER_MISMATCH;
            break;
        }

        frame = (size_t)((uint64_t)physical_address / ZENITH_PAGE_SIZE);
        if (!bitmap_get(eligible_bitmap, frame) ||
            !bitmap_get(used_bitmap, frame) ||
            !bitmap_get(heap_owner_bitmap, frame) ||
            bitmap_get(task_stack_owner_bitmap, frame) ||
            bitmap_get(process_page_owner_bitmap, frame)) {
            status = FRAME_STATUS_OWNER_MISMATCH;
            break;
        }
    }

    for (size_t index = 0U; index < scratch_count; ++index) {
        heap_owner_validation_frames[index] = 0U;
    }

    return status;
}

static enum frame_status
frame_allocator_validate_process_page_owners_unguarded(
    const uintptr_t *committed_frames,
    size_t committed_frame_count,
    const uintptr_t *staged_frames,
    size_t staged_frame_count,
    struct frame_allocator_process_page_certificate *certificate
)
{
    enum frame_status status;
    size_t scratch_count = 0U;
    size_t total_frame_count;

    if (certificate == NULL ||
        (committed_frame_count != 0U && committed_frames == NULL) ||
        (staged_frame_count != 0U && staged_frames == NULL)) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    certificate->mutation_epoch = 0U;
    certificate->owned_frames = 0U;

    if (committed_frame_count >
            FRAME_ALLOCATOR_PROCESS_PAGE_VALIDATION_LIMIT ||
        staged_frame_count >
            FRAME_ALLOCATOR_PROCESS_PAGE_VALIDATION_LIMIT -
                committed_frame_count) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    status = frame_allocator_validate_unguarded();
    if (status != FRAME_STATUS_OK) {
        return status;
    }

    total_frame_count = committed_frame_count + staged_frame_count;
    if (total_frame_count != allocator_stats.process_page_allocated_frames) {
        return FRAME_STATUS_OWNER_MISMATCH;
    }

    for (size_t index = 0U; index < committed_frame_count; ++index) {
        process_page_owner_validation_frames[scratch_count] =
            committed_frames[index];
        ++scratch_count;
    }
    for (size_t index = 0U; index < staged_frame_count; ++index) {
        process_page_owner_validation_frames[scratch_count] =
            staged_frames[index];
        ++scratch_count;
    }

    frame_heap_sort(process_page_owner_validation_frames, scratch_count);

    for (size_t index = 0U; index < scratch_count; ++index) {
        const uintptr_t physical_address =
            process_page_owner_validation_frames[index];
        size_t frame;

        if ((uint64_t)physical_address >= ZENITH_EARLY_PHYSICAL_LIMIT ||
            ((uint64_t)physical_address & (ZENITH_PAGE_SIZE - 1U)) != 0U ||
            (index != 0U &&
             process_page_owner_validation_frames[index - 1U] ==
                physical_address)) {
            status = FRAME_STATUS_OWNER_MISMATCH;
            break;
        }

        frame = (size_t)((uint64_t)physical_address / ZENITH_PAGE_SIZE);
        if (!bitmap_get(eligible_bitmap, frame) ||
            !bitmap_get(used_bitmap, frame) ||
            bitmap_get(heap_owner_bitmap, frame) ||
            bitmap_get(task_stack_owner_bitmap, frame) ||
            !bitmap_get(process_page_owner_bitmap, frame)) {
            status = FRAME_STATUS_OWNER_MISMATCH;
            break;
        }
    }

    for (size_t index = 0U; index < scratch_count; ++index) {
        process_page_owner_validation_frames[index] = 0U;
    }

    if (status == FRAME_STATUS_OK) {
        certificate->mutation_epoch = process_page_mutation_epoch;
        certificate->owned_frames =
            allocator_stats.process_page_allocated_frames;
    }
    return status;
}

static bool frame_test_fail_allocate_after_unguarded(
    size_t successful_allocations
)
{
    if (!allocator_initialized || allocator_poisoned ||
        test_allocation_failure_enabled) {
        return false;
    }

    test_allocation_successes_remaining = successful_allocations;
    test_allocation_failure_enabled = true;
    test_allocation_reports_out_of_memory = false;
    return true;
}

static bool frame_test_report_out_of_memory_after_unguarded(
    size_t successful_allocations
)
{
    if (!allocator_initialized || allocator_poisoned ||
        test_allocation_failure_enabled) {
        return false;
    }

    test_allocation_successes_remaining = successful_allocations;
    test_allocation_failure_enabled = true;
    test_allocation_reports_out_of_memory = true;
    return true;
}

static bool frame_test_set_task_stack_mutation_epoch_unguarded(
    uint64_t mutation_epoch
)
{
    if (!allocator_initialized || allocator_poisoned ||
        mutation_epoch == 0U) {
        return false;
    }

    task_stack_mutation_epoch = mutation_epoch;
    return true;
}

static bool frame_test_set_process_page_mutation_epoch_unguarded(
    uint64_t mutation_epoch
)
{
    if (!allocator_initialized || allocator_poisoned ||
        mutation_epoch == 0U) {
        return false;
    }

    process_page_mutation_epoch = mutation_epoch;
    return true;
}

static bool frame_test_task_stack_state_matches_unguarded(
    size_t expected_owned_frames
)
{
    size_t observed_owned_frames = 0U;

    for (size_t word = 0U; word < BITMAP_WORD_COUNT; ++word) {
        const uint64_t task_owned = task_stack_owner_bitmap[word];

        if ((task_owned & ~used_bitmap[word]) != 0U ||
            (task_owned & heap_owner_bitmap[word]) != 0U ||
            (task_owned & process_page_owner_bitmap[word]) != 0U) {
            return false;
        }
        observed_owned_frames += bitmap_word_population(task_owned);
    }

    return allocator_stats.task_stack_allocated_frames ==
            expected_owned_frames &&
        observed_owned_frames == expected_owned_frames;
}

static bool frame_test_process_page_state_matches_unguarded(
    size_t expected_owned_frames
)
{
    size_t observed_owned_frames = 0U;

    for (size_t word = 0U; word < BITMAP_WORD_COUNT; ++word) {
        const uint64_t process_owned = process_page_owner_bitmap[word];

        if ((process_owned & ~used_bitmap[word]) != 0U ||
            (process_owned & heap_owner_bitmap[word]) != 0U ||
            (process_owned & task_stack_owner_bitmap[word]) != 0U) {
            return false;
        }
        observed_owned_frames += bitmap_word_population(process_owned);
    }

    return allocator_stats.process_page_allocated_frames ==
            expected_owned_frames &&
        observed_owned_frames == expected_owned_frames;
}

static struct frame_allocator_stats frame_allocator_get_stats_unguarded(void)
{
    if (allocator_poisoned) {
        const struct frame_allocator_stats zero = {0U};

        return zero;
    }
    return allocator_stats;
}

static enum frame_status
frame_allocator_task_stack_certificate_get_unguarded(
    struct frame_allocator_task_stack_certificate *certificate
)
{
    if (certificate == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    certificate->mutation_epoch = 0U;
    certificate->owned_frames = 0U;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    certificate->mutation_epoch = task_stack_mutation_epoch;
    certificate->owned_frames = allocator_stats.task_stack_allocated_frames;
    return FRAME_STATUS_OK;
}

static enum frame_status
frame_allocator_process_page_certificate_get_unguarded(
    struct frame_allocator_process_page_certificate *certificate
)
{
    if (certificate == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    certificate->mutation_epoch = 0U;
    certificate->owned_frames = 0U;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (!allocator_stats_are_locally_valid()) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }

    certificate->mutation_epoch = process_page_mutation_epoch;
    certificate->owned_frames =
        allocator_stats.process_page_allocated_frames;
    return FRAME_STATUS_OK;
}

static bool frame_lock_register(void)
{
    enum spinlock_status status;

    if (allocator_lock_registered) {
        return true;
    }

    status = spinlock_runtime_register(
        &allocator_lock,
        FRAME_ALLOCATOR_LOCK_INDEX,
        SPINLOCK_CLASS_PHYSICAL_MEMORY,
        UINT8_C(0)
    );
    if (status != SPINLOCK_STATUS_OK) {
        return false;
    }

    allocator_lock_registered = true;
    return true;
}

static bool frame_lock_acquire(struct spinlock_irq_token *token)
{
    return allocator_lock_registered &&
        spinlock_irqsave_acquire(&allocator_lock, token) ==
            SPINLOCK_STATUS_OK;
}

static enum frame_status frame_lock_failure_status(void)
{
    return allocator_lock_registered
        ? FRAME_STATUS_VALIDATION_FAILURE
        : FRAME_STATUS_NOT_INITIALIZED;
}

static bool frame_lock_exit(struct spinlock_irq_token *token)
{
    if (spinlock_irqrestore_release(&allocator_lock, token) !=
            SPINLOCK_STATUS_OK) {
        frame_allocator_poison();
        return false;
    }
    return true;
}

static enum frame_status frame_lock_finish(
    struct spinlock_irq_token *token,
    enum frame_status status
)
{
    return frame_lock_exit(token) ? status : FRAME_STATUS_VALIDATION_FAILURE;
}

enum frame_status frame_allocator_initialize(const struct boot_context *context)
{
    struct spinlock_irq_token token;

    if (!frame_lock_register() || !frame_lock_acquire(&token)) {
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    return frame_lock_finish(
        &token,
        frame_allocator_initialize_unguarded(context)
    );
}

enum frame_status frame_allocate(uintptr_t *physical_address)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (physical_address != NULL) {
            *physical_address = 0U;
        }
        return frame_lock_failure_status();
    }
    status = frame_allocate_unguarded(physical_address);
    if (!frame_lock_exit(&token)) {
        if (physical_address != NULL) {
            *physical_address = 0U;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && physical_address != NULL) {
        *physical_address = 0U;
    }
    return status;
}

enum frame_status frame_allocate_owned(
    enum frame_owner owner,
    uintptr_t *physical_address
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (physical_address != NULL) {
            *physical_address = 0U;
        }
        return frame_lock_failure_status();
    }
    status = frame_allocate_owned_unguarded(owner, physical_address);
    if (!frame_lock_exit(&token)) {
        if (physical_address != NULL) {
            *physical_address = 0U;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && physical_address != NULL) {
        *physical_address = 0U;
    }
    return status;
}

enum frame_status frame_release(uintptr_t physical_address)
{
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return frame_lock_failure_status();
    }
    return frame_lock_finish(
        &token,
        frame_release_unguarded(physical_address)
    );
}

enum frame_status frame_release_owned(
    uintptr_t physical_address,
    enum frame_owner owner
)
{
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return frame_lock_failure_status();
    }
    return frame_lock_finish(
        &token,
        frame_release_owned_unguarded(physical_address, owner)
    );
}

enum frame_status frame_query_allocation(
    uintptr_t physical_address,
    bool *allocated
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (allocated != NULL) {
            *allocated = false;
        }
        return frame_lock_failure_status();
    }
    status = frame_query_allocation_unguarded(physical_address, allocated);
    if (!frame_lock_exit(&token)) {
        if (allocated != NULL) {
            *allocated = false;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && allocated != NULL) {
        *allocated = false;
    }
    return status;
}

enum frame_status frame_query_owner(
    uintptr_t physical_address,
    enum frame_owner *owner
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (owner != NULL) {
            *owner = FRAME_OWNER_NONE;
        }
        return frame_lock_failure_status();
    }
    status = frame_query_owner_unguarded(physical_address, owner);
    if (!frame_lock_exit(&token)) {
        if (owner != NULL) {
            *owner = FRAME_OWNER_NONE;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && owner != NULL) {
        *owner = FRAME_OWNER_NONE;
    }
    return status;
}

enum frame_status frame_reserve_range(uint64_t base_address, uint64_t length)
{
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return frame_lock_failure_status();
    }
    return frame_lock_finish(
        &token,
        frame_reserve_range_unguarded(base_address, length)
    );
}

enum frame_status frame_allocator_validate(void)
{
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return frame_lock_failure_status();
    }
    return frame_lock_finish(&token, frame_allocator_validate_unguarded());
}

bool frame_test_fail_allocate_after(size_t successful_allocations)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_fail_allocate_after_unguarded(successful_allocations);
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

bool frame_test_report_out_of_memory_after(size_t successful_allocations)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_report_out_of_memory_after_unguarded(
        successful_allocations
    );
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

bool frame_test_set_task_stack_mutation_epoch(uint64_t mutation_epoch)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_set_task_stack_mutation_epoch_unguarded(
        mutation_epoch
    );
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

bool frame_test_task_stack_state_matches(size_t expected_owned_frames)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_task_stack_state_matches_unguarded(
        expected_owned_frames
    );
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

bool frame_test_set_process_page_mutation_epoch(uint64_t mutation_epoch)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_set_process_page_mutation_epoch_unguarded(
        mutation_epoch
    );
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

bool frame_test_process_page_state_matches(size_t expected_owned_frames)
{
    struct spinlock_irq_token token;
    bool result;

    if (!frame_lock_acquire(&token)) {
        return false;
    }
    result = frame_test_process_page_state_matches_unguarded(
        expected_owned_frames
    );
    if (!frame_lock_exit(&token)) {
        return false;
    }
    return result;
}

struct frame_allocator_stats frame_allocator_get_stats(void)
{
    struct frame_allocator_stats stats = {0U};
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return stats;
    }
    stats = frame_allocator_get_stats_unguarded();
    if (!frame_lock_exit(&token)) {
        struct frame_allocator_stats zero = {0U};

        return zero;
    }
    return stats;
}

enum frame_status frame_allocator_task_stack_certificate_get(
    struct frame_allocator_task_stack_certificate *certificate
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_task_stack_certificate zero = {0U};

            *certificate = zero;
        }
        return frame_lock_failure_status();
    }
    status = frame_allocator_task_stack_certificate_get_unguarded(
        certificate
    );
    if (!frame_lock_exit(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_task_stack_certificate zero = {0U};

            *certificate = zero;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && certificate != NULL) {
        const struct frame_allocator_task_stack_certificate zero = {0U};

        *certificate = zero;
    }
    return status;
}

enum frame_status frame_allocator_process_page_certificate_get(
    struct frame_allocator_process_page_certificate *certificate
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_process_page_certificate zero = {
                0U
            };

            *certificate = zero;
        }
        return frame_lock_failure_status();
    }
    status = frame_allocator_process_page_certificate_get_unguarded(
        certificate
    );
    if (!frame_lock_exit(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_process_page_certificate zero = {
                0U
            };

            *certificate = zero;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && certificate != NULL) {
        const struct frame_allocator_process_page_certificate zero = {0U};

        *certificate = zero;
    }
    return status;
}

enum frame_status frame_allocator_validate_heap_owners(
    const uintptr_t *committed_frames,
    size_t committed_frame_count,
    const uintptr_t *staged_frames,
    size_t staged_frame_count
)
{
    struct spinlock_irq_token token;

    if (!frame_lock_acquire(&token)) {
        return frame_lock_failure_status();
    }
    return frame_lock_finish(
        &token,
        frame_allocator_validate_heap_owners_unguarded(
            committed_frames,
            committed_frame_count,
            staged_frames,
            staged_frame_count
        )
    );
}

enum frame_status frame_allocator_validate_process_page_owners(
    const uintptr_t *committed_frames,
    size_t committed_frame_count,
    const uintptr_t *staged_frames,
    size_t staged_frame_count,
    struct frame_allocator_process_page_certificate *certificate
)
{
    struct spinlock_irq_token token;
    enum frame_status status;

    if (!frame_lock_acquire(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_process_page_certificate zero = {
                0U
            };

            *certificate = zero;
        }
        return frame_lock_failure_status();
    }
    status = frame_allocator_validate_process_page_owners_unguarded(
        committed_frames,
        committed_frame_count,
        staged_frames,
        staged_frame_count,
        certificate
    );
    if (!frame_lock_exit(&token)) {
        if (certificate != NULL) {
            const struct frame_allocator_process_page_certificate zero = {
                0U
            };

            *certificate = zero;
        }
        return FRAME_STATUS_VALIDATION_FAILURE;
    }
    if (status != FRAME_STATUS_OK && certificate != NULL) {
        const struct frame_allocator_process_page_certificate zero = {0U};

        *certificate = zero;
    }
    return status;
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

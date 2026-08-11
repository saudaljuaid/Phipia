/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/heap.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/spinlock.h>
#include <zenith/virtual_memory.h>

#include "heap_core.h"

#define HEAP_PAGE_COUNT \
    ((size_t)(VIRTUAL_MEMORY_HEAP_SIZE / ZENITH_PAGE_SIZE))
#define HEAP_LOCK_INDEX UINT32_C(2)

_Static_assert(sizeof(uintptr_t) == sizeof(uint64_t),
    "the kernel heap requires 64-bit pointers");
_Static_assert(VIRTUAL_MEMORY_HEAP_SIZE % ZENITH_PAGE_SIZE == 0U,
    "the kernel heap window must contain whole pages");
_Static_assert(VIRTUAL_MEMORY_HEAP_BASE % ZENITH_PAGE_SIZE == 0U,
    "the kernel heap base must be page aligned");
_Static_assert(HEAP_PAGE_COUNT == FRAME_ALLOCATOR_HEAP_VALIDATION_LIMIT,
    "heap and physical batch-validation limits must agree");
_Static_assert(VIRTUAL_MEMORY_HEAP_SIZE <= SIZE_MAX,
    "the kernel heap window must fit in size_t");
_Static_assert(SPINLOCK_CLASS_HEAP < SPINLOCK_CLASS_VIRTUAL_MEMORY &&
    SPINLOCK_CLASS_VIRTUAL_MEMORY < SPINLOCK_CLASS_PHYSICAL_MEMORY,
    "heap transactions must nest through VM into physical memory");

static struct heap_core_state active_core;
static struct heap_core_state candidate_core;
static struct heap_core_state self_test_core;
static uintptr_t backing_frames[HEAP_PAGE_COUNT];
static uintptr_t transaction_frames[HEAP_PAGE_COUNT];
static bool transaction_mapped[HEAP_PAGE_COUNT];
static size_t failed_allocations;
static bool heap_initialized;
static bool heap_poisoned;
static bool heap_lock_registered;
static struct spinlock heap_lock;

static bool forbidden_context(void)
{
    return interrupt_context_active() || console_panic_active();
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (right > UINT64_MAX - left) {
        return false;
    }

    *sum = left + right;
    return true;
}

static bool round_up_pages(uint64_t bytes, uint64_t *rounded_bytes)
{
    const uint64_t mask = ZENITH_PAGE_SIZE - 1U;

    if (bytes == 0U || bytes > UINT64_MAX - mask) {
        return false;
    }

    *rounded_bytes = (bytes + mask) & ~mask;
    return true;
}

static enum virtual_memory_status map_page_interrupt_safe(
    uint64_t virtual_address,
    uintptr_t physical_address
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum virtual_memory_status status;

    cpu_interrupt_disable();
    status = virtual_memory_map_heap_page(
        virtual_address,
        physical_address
    );

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    return status;
}

static enum virtual_memory_status unmap_page_interrupt_safe(
    uint64_t virtual_address,
    uintptr_t physical_address
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum virtual_memory_status status;

    cpu_interrupt_disable();
    status = virtual_memory_unmap_heap_page(
        virtual_address,
        physical_address
    );

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    return status;
}

static enum heap_status core_status_to_heap(enum heap_core_status status)
{
    switch (status) {
    case HEAP_CORE_STATUS_OK:
        return HEAP_STATUS_OK;
    case HEAP_CORE_STATUS_NULL_ARGUMENT:
        return HEAP_STATUS_NULL_ARGUMENT;
    case HEAP_CORE_STATUS_ALREADY_INITIALIZED:
        return HEAP_STATUS_ALREADY_INITIALIZED;
    case HEAP_CORE_STATUS_NOT_INITIALIZED:
        return HEAP_STATUS_NOT_INITIALIZED;
    case HEAP_CORE_STATUS_BAD_CAPACITY:
    case HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW:
        return HEAP_STATUS_ARITHMETIC_OVERFLOW;
    case HEAP_CORE_STATUS_ZERO_SIZE:
        return HEAP_STATUS_ZERO_SIZE;
    case HEAP_CORE_STATUS_INVALID_ALIGNMENT:
        return HEAP_STATUS_INVALID_ALIGNMENT;
    case HEAP_CORE_STATUS_OUT_OF_SPACE:
        return HEAP_STATUS_WINDOW_EXHAUSTED;
    case HEAP_CORE_STATUS_DESCRIPTOR_LIMIT:
        return HEAP_STATUS_DESCRIPTOR_LIMIT;
    case HEAP_CORE_STATUS_POINTER_OUTSIDE:
        return HEAP_STATUS_POINTER_OUTSIDE_HEAP;
    case HEAP_CORE_STATUS_MISALIGNED_POINTER:
        return HEAP_STATUS_MISALIGNED_POINTER;
    case HEAP_CORE_STATUS_INTERIOR_POINTER:
        return HEAP_STATUS_INTERIOR_POINTER;
    case HEAP_CORE_STATUS_INVALID_POINTER:
        return HEAP_STATUS_INVALID_POINTER;
    case HEAP_CORE_STATUS_DOUBLE_FREE:
        return HEAP_STATUS_DOUBLE_FREE;
    case HEAP_CORE_STATUS_CORRUPTED_STATE:
        return HEAP_STATUS_CORRUPTED_STATE;
    case HEAP_CORE_STATUS_STATS_INVALID:
        return HEAP_STATUS_STATS_INVALID;
    default:
        return HEAP_STATUS_CORRUPTED_STATE;
    }
}

static enum heap_status record_allocation_failure(enum heap_status status)
{
    if (heap_initialized && !heap_poisoned) {
        if (failed_allocations == SIZE_MAX) {
            heap_poisoned = true;
            return HEAP_STATUS_STATS_INVALID;
        }

        ++failed_allocations;
    }

    return status;
}

static enum heap_status validate_candidate(
    const struct heap_core_state *candidate,
    size_t old_page_count,
    size_t added_page_count
)
{
    struct frame_allocator_stats frame_stats;
    struct heap_core_stats core_stats;
    struct virtual_memory_runtime_stats vm_stats;
    size_t total_pages;
    enum heap_core_status core_status;
    enum frame_status frame_status;
    enum virtual_memory_status vm_status;

    if (old_page_count > HEAP_PAGE_COUNT ||
        added_page_count > HEAP_PAGE_COUNT - old_page_count) {
        return HEAP_STATUS_STATS_INVALID;
    }

    total_pages = old_page_count + added_page_count;

    core_status = heap_core_get_stats(candidate, &core_stats);

    if (core_status != HEAP_CORE_STATUS_OK) {
        return core_status == HEAP_CORE_STATUS_STATS_INVALID
            ? HEAP_STATUS_STATS_INVALID
            : HEAP_STATUS_CORRUPTED_STATE;
    }

    if (core_stats.committed != (uint64_t)total_pages * ZENITH_PAGE_SIZE) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    frame_status = frame_allocator_validate_heap_owners(
        backing_frames,
        old_page_count,
        transaction_frames,
        added_page_count
    );
    if (frame_status == FRAME_STATUS_OWNER_MISMATCH) {
        return HEAP_STATUS_MAPPING_FAILURE;
    }
    if (frame_status != FRAME_STATUS_OK) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    vm_status = virtual_memory_validate_heap_backing(
        backing_frames,
        old_page_count,
        transaction_frames,
        added_page_count,
        &vm_stats
    );

    if (vm_status == VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH) {
        return HEAP_STATUS_MAPPING_FAILURE;
    }

    if (vm_status != VIRTUAL_MEMORY_STATUS_OK ||
        vm_stats.heap_window_pages != HEAP_PAGE_COUNT ||
        vm_stats.heap_mapped_pages != total_pages ||
        vm_stats.table_pages_used > vm_stats.table_pages_capacity) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    frame_stats = frame_allocator_get_stats();

    if (frame_stats.heap_allocated_frames != total_pages ||
        frame_stats.allocated_frames !=
            frame_stats.generic_allocated_frames +
                frame_stats.heap_allocated_frames +
                frame_stats.task_stack_allocated_frames) {
        return HEAP_STATUS_STATS_INVALID;
    }

    for (size_t index = 0U; index < added_page_count; ++index) {
        if (!transaction_mapped[index]) {
            return HEAP_STATUS_MAPPING_FAILURE;
        }
    }

    return HEAP_STATUS_OK;
}

static void clear_transaction(size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        transaction_frames[index] = 0U;
        transaction_mapped[index] = false;
    }
}

static enum heap_status rollback_transaction(
    size_t old_page_count,
    size_t allocated_count
)
{
    bool rollback_failed = false;
    struct frame_allocator_stats frame_stats;

    for (size_t count = allocated_count; count > 0U; --count) {
        const size_t index = count - 1U;

        if (transaction_mapped[index]) {
            enum virtual_memory_status vm_status = unmap_page_interrupt_safe(
                VIRTUAL_MEMORY_HEAP_BASE +
                    (uint64_t)(old_page_count + index) * ZENITH_PAGE_SIZE,
                transaction_frames[index]
            );

            if (vm_status == VIRTUAL_MEMORY_STATUS_OK) {
                transaction_mapped[index] = false;
            } else {
                rollback_failed = true;
            }
        }

        if (!transaction_mapped[index] && transaction_frames[index] != 0U) {
            if (frame_release_owned(
                    transaction_frames[index],
                    FRAME_OWNER_KERNEL_HEAP
                ) != FRAME_STATUS_OK) {
                rollback_failed = true;
            } else {
                transaction_frames[index] = 0U;
            }
        }
    }

    if (rollback_failed) {
        heap_poisoned = true;
        return HEAP_STATUS_ROLLBACK_FAILURE;
    }

    clear_transaction(allocated_count);

    frame_stats = frame_allocator_get_stats();

    if (frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK ||
        frame_stats.heap_allocated_frames != old_page_count ||
        frame_stats.allocated_frames !=
            frame_stats.generic_allocated_frames +
                frame_stats.heap_allocated_frames +
                frame_stats.task_stack_allocated_frames) {
        heap_poisoned = true;
        return HEAP_STATUS_ROLLBACK_FAILURE;
    }

    return HEAP_STATUS_OK;
}

static enum heap_status acquire_and_map_pages(
    size_t old_page_count,
    size_t additional_page_count
)
{
    size_t allocated_count = 0U;

    clear_transaction(additional_page_count);

    for (size_t index = 0U; index < additional_page_count; ++index) {
        enum frame_status frame_status;
        enum virtual_memory_status vm_status;
        volatile uint8_t *page;

        frame_status = frame_allocate_owned(
            FRAME_OWNER_KERNEL_HEAP,
            &transaction_frames[index]
        );

        if (frame_status != FRAME_STATUS_OK) {
            enum heap_status rollback_status = rollback_transaction(
                old_page_count,
                allocated_count
            );

            if (rollback_status != HEAP_STATUS_OK) {
                return rollback_status;
            }

            return frame_status == FRAME_STATUS_OUT_OF_MEMORY
                ? HEAP_STATUS_OUT_OF_MEMORY
                : frame_status == FRAME_STATUS_TEST_FAILURE
                    ? HEAP_STATUS_TEST_FAILURE
                    : HEAP_STATUS_FRAME_FAILURE;
        }

        ++allocated_count;
        vm_status = map_page_interrupt_safe(
            VIRTUAL_MEMORY_HEAP_BASE +
                (uint64_t)(old_page_count + index) * ZENITH_PAGE_SIZE,
            transaction_frames[index]
        );

        if (vm_status != VIRTUAL_MEMORY_STATUS_OK) {
            /*
             * A mapper rollback failure leaves the leaf state uncertain.  Make
             * the outer rollback treat this frame as potentially mapped so it
             * can never be released before an exact unmap succeeds.
             */
            if (vm_status == VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE) {
                transaction_mapped[index] = true;
            }

            enum heap_status rollback_status = rollback_transaction(
                old_page_count,
                allocated_count
            );

            if (rollback_status != HEAP_STATUS_OK) {
                return rollback_status;
            }

            return vm_status == VIRTUAL_MEMORY_STATUS_TEST_FAILURE
                ? HEAP_STATUS_TEST_FAILURE
                : HEAP_STATUS_MAPPING_FAILURE;
        }

        transaction_mapped[index] = true;
        page = (volatile uint8_t *)(uintptr_t)(
            VIRTUAL_MEMORY_HEAP_BASE +
            (uint64_t)(old_page_count + index) * ZENITH_PAGE_SIZE
        );

        for (size_t byte = 0U; byte < (size_t)ZENITH_PAGE_SIZE; ++byte) {
            page[byte] = 0U;
        }
    }

    return HEAP_STATUS_OK;
}

static enum heap_status validate_active(void)
{
    struct heap_core_stats stats;
    const struct frame_allocator_stats frame_stats =
        frame_allocator_get_stats();
    struct virtual_memory_runtime_stats vm_stats;
    size_t mapped_pages;
    enum heap_core_status core_status;
    enum frame_status frame_status;
    enum virtual_memory_status vm_status;

    if (!heap_initialized) {
        return HEAP_STATUS_NOT_INITIALIZED;
    }

    if (heap_poisoned) {
        return HEAP_STATUS_CORRUPTED_STATE;
    }

    core_status = heap_core_get_stats(&active_core, &stats);

    if (core_status != HEAP_CORE_STATUS_OK) {
        return core_status == HEAP_CORE_STATUS_STATS_INVALID
            ? HEAP_STATUS_STATS_INVALID
            : HEAP_STATUS_CORRUPTED_STATE;
    }

    if (stats.capacity != VIRTUAL_MEMORY_HEAP_SIZE ||
        stats.committed % ZENITH_PAGE_SIZE != 0U ||
        stats.committed > stats.capacity ||
        stats.live_requested_bytes > stats.live_extent_bytes ||
        stats.live_extent_bytes + stats.reusable_bytes != stats.committed ||
        stats.largest_reusable_block > stats.reusable_bytes ||
        stats.descriptors_in_use > stats.descriptor_limit ||
        stats.descriptor_limit != HEAP_DESCRIPTOR_LIMIT) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    mapped_pages = (size_t)(stats.committed / ZENITH_PAGE_SIZE);
    frame_status = frame_allocator_validate_heap_owners(
        backing_frames,
        mapped_pages,
        NULL,
        0U
    );
    if (frame_status == FRAME_STATUS_OWNER_MISMATCH) {
        return HEAP_STATUS_MAPPING_FAILURE;
    }
    if (frame_status != FRAME_STATUS_OK) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    vm_status = virtual_memory_validate_heap_backing(
        backing_frames,
        mapped_pages,
        NULL,
        0U,
        &vm_stats
    );

    if (vm_status == VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH) {
        return HEAP_STATUS_MAPPING_FAILURE;
    }

    if (vm_status != VIRTUAL_MEMORY_STATUS_OK) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    if (mapped_pages > HEAP_PAGE_COUNT ||
        vm_stats.heap_window_pages != HEAP_PAGE_COUNT ||
        vm_stats.heap_mapped_pages != mapped_pages ||
        vm_stats.table_pages_used > vm_stats.table_pages_capacity ||
        frame_stats.heap_allocated_frames != mapped_pages ||
        frame_stats.allocated_frames !=
            frame_stats.generic_allocated_frames +
                frame_stats.heap_allocated_frames +
                frame_stats.task_stack_allocated_frames) {
        return HEAP_STATUS_STATS_INVALID;
    }

    for (size_t index = mapped_pages; index < HEAP_PAGE_COUNT; ++index) {
        if (backing_frames[index] != 0U) {
            return HEAP_STATUS_MAPPING_FAILURE;
        }
    }

    return HEAP_STATUS_OK;
}

static bool core_self_test(void)
{
    struct heap_core_stats stats;
    uint64_t first;
    uint64_t second;
    uint64_t third;
    uint64_t growth;

    heap_core_clear(&self_test_core);

    if (heap_core_initialize(&self_test_core, UINT64_C(0x8000)) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_initialize(&self_test_core, UINT64_C(0x8000)) !=
            HEAP_CORE_STATUS_ALREADY_INITIALIZED ||
        heap_core_allocate(&self_test_core, 0U, 1U, &first) !=
            HEAP_CORE_STATUS_ZERO_SIZE ||
        heap_core_allocate(&self_test_core, 1U, 0U, &first) !=
            HEAP_CORE_STATUS_INVALID_ALIGNMENT ||
        heap_core_allocate(&self_test_core, 1U, 3U, &first) !=
            HEAP_CORE_STATUS_INVALID_ALIGNMENT ||
        heap_core_allocate(&self_test_core, 1U, 8192U, &first) !=
            HEAP_CORE_STATUS_INVALID_ALIGNMENT ||
        heap_core_growth_needed(
            &self_test_core,
            UINT64_MAX,
            4096U,
            &growth
        ) != HEAP_CORE_STATUS_OUT_OF_SPACE ||
        heap_core_grow(&self_test_core, UINT64_C(0x4000)) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_allocate(&self_test_core, 32U, 1U, &first) !=
            HEAP_CORE_STATUS_OK ||
        first != 0U ||
        heap_core_growth_needed(
            &self_test_core,
            UINT64_MAX,
            16U,
            &growth
        ) != HEAP_CORE_STATUS_ARITHMETIC_OVERFLOW ||
        heap_core_allocate(&self_test_core, 64U, 256U, &second) !=
            HEAP_CORE_STATUS_OK ||
        second != 256U ||
        heap_core_allocate(&self_test_core, 128U, 16U, &third) !=
            HEAP_CORE_STATUS_OK ||
        third == first || third == second ||
        heap_core_deallocate(&self_test_core, first + 16U) !=
            HEAP_CORE_STATUS_INTERIOR_POINTER ||
        heap_core_deallocate(&self_test_core, first + 1U) !=
            HEAP_CORE_STATUS_MISALIGNED_POINTER ||
        heap_core_deallocate(&self_test_core, second) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_deallocate(&self_test_core, second) !=
            HEAP_CORE_STATUS_DOUBLE_FREE ||
        heap_core_deallocate(&self_test_core, first) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_deallocate(&self_test_core, third) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_get_stats(&self_test_core, &stats) !=
            HEAP_CORE_STATUS_OK ||
        stats.live_allocations != 0U ||
        stats.reusable_bytes != UINT64_C(0x4000) ||
        stats.largest_reusable_block != UINT64_C(0x4000) ||
        stats.descriptors_in_use != 1U ||
        heap_core_allocate(
            &self_test_core,
            UINT64_C(0x4000),
            4096U,
            &first
        ) != HEAP_CORE_STATUS_OK ||
        first != 0U ||
        heap_core_allocate(&self_test_core, 1U, 1U, &second) !=
            HEAP_CORE_STATUS_OUT_OF_SPACE ||
        heap_core_deallocate(&self_test_core, first) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_growth_needed(
            &self_test_core,
            UINT64_C(0x4001),
            1U,
            &growth
        ) != HEAP_CORE_STATUS_OK ||
        growth != 1U ||
        heap_core_grow(&self_test_core, UINT64_C(0x4001)) !=
            HEAP_CORE_STATUS_OUT_OF_SPACE) {
        return false;
    }

    heap_core_clear(&self_test_core);

    if (heap_core_initialize(&self_test_core, UINT64_C(0x2000)) !=
            HEAP_CORE_STATUS_OK ||
        heap_core_grow(&self_test_core, UINT64_C(0x2000)) !=
            HEAP_CORE_STATUS_OK) {
        return false;
    }

    for (size_t index = 0U; index < HEAP_DESCRIPTOR_LIMIT / 2U; ++index) {
        if (heap_core_allocate(&self_test_core, 1U, 1U, &first) !=
                HEAP_CORE_STATUS_OK ||
            first != (uint64_t)index * HEAP_MINIMUM_ALIGNMENT) {
            return false;
        }
    }

    if (heap_core_allocate(&self_test_core, 1U, 1U, &first) !=
            HEAP_CORE_STATUS_DESCRIPTOR_LIMIT ||
        heap_core_get_stats(&self_test_core, &stats) !=
            HEAP_CORE_STATUS_OK ||
        stats.descriptors_in_use != HEAP_DESCRIPTOR_LIMIT ||
        stats.live_allocations != HEAP_DESCRIPTOR_LIMIT / 2U) {
        return false;
    }

    for (size_t index = 0U; index < HEAP_DESCRIPTOR_LIMIT / 2U; ++index) {
        if (heap_core_deallocate(
                &self_test_core,
                (uint64_t)index * HEAP_MINIMUM_ALIGNMENT
            ) != HEAP_CORE_STATUS_OK) {
            return false;
        }
    }

    if (heap_core_get_stats(&self_test_core, &stats) !=
            HEAP_CORE_STATUS_OK ||
        stats.descriptors_in_use != 1U || stats.live_allocations != 0U ||
        stats.largest_reusable_block != UINT64_C(0x2000)) {
        return false;
    }

    self_test_core.descriptors[self_test_core.head].integrity ^= 1U;
    return heap_core_validate(&self_test_core) ==
        HEAP_CORE_STATUS_CORRUPTED_STATE;
}

bool heap_self_test(void)
{
    uint64_t rounded;

    return !heap_initialized &&
        !round_up_pages(0U, &rounded) &&
        round_up_pages(1U, &rounded) && rounded == ZENITH_PAGE_SIZE &&
        round_up_pages(ZENITH_PAGE_SIZE, &rounded) &&
            rounded == ZENITH_PAGE_SIZE &&
        round_up_pages(ZENITH_PAGE_SIZE + 1U, &rounded) &&
            rounded == ZENITH_PAGE_SIZE * 2U &&
        !round_up_pages(UINT64_MAX, &rounded) &&
        core_self_test();
}

static enum heap_status heap_initialize_unguarded(void)
{
    struct frame_allocator_stats frame_stats;
    struct virtual_memory_runtime_stats vm_stats;

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (heap_initialized) {
        return HEAP_STATUS_ALREADY_INITIALIZED;
    }

    if (frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_runtime_get_stats(&vm_stats) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        vm_stats.heap_window_pages != HEAP_PAGE_COUNT ||
        vm_stats.heap_mapped_pages != 0U) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    frame_stats = frame_allocator_get_stats();

    if (frame_stats.heap_allocated_frames != 0U ||
        frame_stats.task_stack_allocated_frames != 0U ||
        frame_stats.generic_allocated_frames != frame_stats.allocated_frames) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    heap_core_clear(&active_core);
    heap_core_clear(&candidate_core);

    if (heap_core_initialize(&active_core, VIRTUAL_MEMORY_HEAP_SIZE) !=
        HEAP_CORE_STATUS_OK) {
        return HEAP_STATUS_CORRUPTED_STATE;
    }

    for (size_t index = 0U; index < HEAP_PAGE_COUNT; ++index) {
        backing_frames[index] = 0U;
        transaction_frames[index] = 0U;
        transaction_mapped[index] = false;
    }

    failed_allocations = 0U;
    heap_poisoned = false;
    heap_initialized = true;

    if (validate_active() != HEAP_STATUS_OK) {
        heap_initialized = false;
        heap_core_clear(&active_core);
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    return HEAP_STATUS_OK;
}

static enum heap_status heap_allocate_unguarded(
    size_t byte_size,
    size_t alignment,
    void **allocation
)
{
    struct heap_core_stats active_stats;
    uint64_t offset;
    uint64_t required_growth;
    uint64_t rounded_growth;
    uint64_t virtual_address;
    size_t old_page_count;
    size_t added_page_count;
    enum heap_core_status core_status;
    enum heap_status status;

    if (allocation == NULL) {
        return record_allocation_failure(HEAP_STATUS_NULL_ARGUMENT);
    }

    *allocation = NULL;

    if (forbidden_context()) {
        return record_allocation_failure(HEAP_STATUS_FORBIDDEN_CONTEXT);
    }

    status = validate_active();

    if (status != HEAP_STATUS_OK) {
        return record_allocation_failure(status);
    }

    heap_core_copy(&candidate_core, &active_core);
    core_status = heap_core_allocate(
        &candidate_core,
        (uint64_t)byte_size,
        (uint64_t)alignment,
        &offset
    );

    if (core_status == HEAP_CORE_STATUS_OK) {
        if (!add_u64(VIRTUAL_MEMORY_HEAP_BASE, offset, &virtual_address) ||
            virtual_address >= VIRTUAL_MEMORY_HEAP_GUARD_ABOVE) {
            return record_allocation_failure(
                HEAP_STATUS_ARITHMETIC_OVERFLOW
            );
        }

        heap_core_copy(&active_core, &candidate_core);
        *allocation = (void *)(uintptr_t)virtual_address;
        return HEAP_STATUS_OK;
    }

    if (core_status != HEAP_CORE_STATUS_OUT_OF_SPACE) {
        return record_allocation_failure(core_status_to_heap(core_status));
    }

    core_status = heap_core_growth_needed(
        &active_core,
        (uint64_t)byte_size,
        (uint64_t)alignment,
        &required_growth
    );

    if (core_status != HEAP_CORE_STATUS_OK) {
        return record_allocation_failure(core_status_to_heap(core_status));
    }

    if (!round_up_pages(required_growth, &rounded_growth) ||
        rounded_growth > VIRTUAL_MEMORY_HEAP_SIZE) {
        return record_allocation_failure(HEAP_STATUS_ARITHMETIC_OVERFLOW);
    }

    if (heap_core_get_stats(&active_core, &active_stats) !=
            HEAP_CORE_STATUS_OK ||
        active_stats.committed / ZENITH_PAGE_SIZE > HEAP_PAGE_COUNT) {
        return record_allocation_failure(HEAP_STATUS_CORRUPTED_STATE);
    }

    old_page_count = (size_t)(active_stats.committed / ZENITH_PAGE_SIZE);
    added_page_count = (size_t)(rounded_growth / ZENITH_PAGE_SIZE);

    if (added_page_count == 0U ||
        added_page_count > HEAP_PAGE_COUNT - old_page_count) {
        return record_allocation_failure(HEAP_STATUS_WINDOW_EXHAUSTED);
    }

    heap_core_copy(&candidate_core, &active_core);
    core_status = heap_core_grow(&candidate_core, rounded_growth);

    if (core_status == HEAP_CORE_STATUS_OK) {
        core_status = heap_core_allocate(
            &candidate_core,
            (uint64_t)byte_size,
            (uint64_t)alignment,
            &offset
        );
    }

    if (core_status != HEAP_CORE_STATUS_OK) {
        return record_allocation_failure(core_status_to_heap(core_status));
    }

    status = acquire_and_map_pages(old_page_count, added_page_count);

    if (status != HEAP_STATUS_OK) {
        return record_allocation_failure(status);
    }

    status = validate_candidate(
        &candidate_core,
        old_page_count,
        added_page_count
    );

    if (status != HEAP_STATUS_OK) {
        enum heap_status rollback_status = rollback_transaction(
            old_page_count,
            added_page_count
        );

        return record_allocation_failure(
            rollback_status == HEAP_STATUS_OK ? status : rollback_status
        );
    }

    if (!add_u64(VIRTUAL_MEMORY_HEAP_BASE, offset, &virtual_address) ||
        virtual_address >= VIRTUAL_MEMORY_HEAP_GUARD_ABOVE) {
        enum heap_status rollback_status = rollback_transaction(
            old_page_count,
            added_page_count
        );

        return record_allocation_failure(
            rollback_status == HEAP_STATUS_OK
                ? HEAP_STATUS_ARITHMETIC_OVERFLOW
                : rollback_status
        );
    }

    for (size_t index = 0U; index < added_page_count; ++index) {
        backing_frames[old_page_count + index] = transaction_frames[index];
        transaction_frames[index] = 0U;
        transaction_mapped[index] = false;
    }

    heap_core_copy(&active_core, &candidate_core);
    *allocation = (void *)(uintptr_t)virtual_address;
    return HEAP_STATUS_OK;
}

static enum heap_status heap_deallocate_unguarded(void *allocation)
{
    struct heap_core_stats stats;
    uintptr_t address;
    uint64_t heap_end;
    uint64_t offset;
    enum heap_core_status core_status;
    enum heap_status status;

    if (allocation == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_active();

    if (status != HEAP_STATUS_OK) {
        return status;
    }

    if (heap_core_get_stats(&active_core, &stats) != HEAP_CORE_STATUS_OK ||
        !add_u64(VIRTUAL_MEMORY_HEAP_BASE, stats.committed, &heap_end)) {
        return HEAP_STATUS_CORRUPTED_STATE;
    }

    address = (uintptr_t)allocation;

    if ((uint64_t)address < VIRTUAL_MEMORY_HEAP_BASE ||
        (uint64_t)address >= heap_end) {
        return HEAP_STATUS_POINTER_OUTSIDE_HEAP;
    }

    offset = (uint64_t)address - VIRTUAL_MEMORY_HEAP_BASE;

    if ((offset & ((uint64_t)HEAP_MINIMUM_ALIGNMENT - 1U)) != 0U) {
        return HEAP_STATUS_MISALIGNED_POINTER;
    }

    heap_core_copy(&candidate_core, &active_core);
    core_status = heap_core_deallocate(&candidate_core, offset);

    if (core_status != HEAP_CORE_STATUS_OK) {
        return core_status_to_heap(core_status);
    }

    heap_core_copy(&active_core, &candidate_core);
    return HEAP_STATUS_OK;
}

static enum heap_status heap_validate_unguarded(void)
{
    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    return validate_active();
}

static enum heap_status heap_get_stats_unguarded(struct heap_stats *stats)
{
    struct heap_core_stats core_stats;
    enum heap_status status;

    if (stats == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }

    stats->capacity_bytes = 0U;
    stats->committed_bytes = 0U;
    stats->mapped_pages = 0U;
    stats->live_allocations = 0U;
    stats->live_requested_bytes = 0U;
    stats->live_extent_bytes = 0U;
    stats->reusable_bytes = 0U;
    stats->largest_reusable_block = 0U;
    stats->descriptors_in_use = 0U;
    stats->descriptor_limit = 0U;
    stats->failed_allocations = 0U;

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_active();

    if (status != HEAP_STATUS_OK) {
        return status;
    }

    if (heap_core_get_stats(&active_core, &core_stats) !=
        HEAP_CORE_STATUS_OK) {
        return HEAP_STATUS_STATS_INVALID;
    }

    stats->capacity_bytes = (size_t)core_stats.capacity;
    stats->committed_bytes = (size_t)core_stats.committed;
    stats->mapped_pages = (size_t)(core_stats.committed / ZENITH_PAGE_SIZE);
    stats->live_allocations = (size_t)core_stats.live_allocations;
    stats->live_requested_bytes = (size_t)core_stats.live_requested_bytes;
    stats->live_extent_bytes = (size_t)core_stats.live_extent_bytes;
    stats->reusable_bytes = (size_t)core_stats.reusable_bytes;
    stats->largest_reusable_block =
        (size_t)core_stats.largest_reusable_block;
    stats->descriptors_in_use = (size_t)core_stats.descriptors_in_use;
    stats->descriptor_limit = (size_t)core_stats.descriptor_limit;
    stats->failed_allocations = failed_allocations;
    return HEAP_STATUS_OK;
}

static bool heap_lock_register(void)
{
    enum spinlock_status status;

    if (heap_lock_registered) {
        return true;
    }

    status = spinlock_runtime_register(
        &heap_lock,
        HEAP_LOCK_INDEX,
        SPINLOCK_CLASS_HEAP,
        UINT8_C(0)
    );
    if (status != SPINLOCK_STATUS_OK) {
        return false;
    }

    heap_lock_registered = true;
    return true;
}

static bool heap_lock_acquire(struct spinlock_irq_token *token)
{
    return heap_lock_registered && !heap_poisoned &&
        spinlock_irqsave_acquire(&heap_lock, token) == SPINLOCK_STATUS_OK;
}

static enum heap_status heap_lock_failure_status(void)
{
    return heap_lock_registered
        ? HEAP_STATUS_CORRUPTED_STATE
        : HEAP_STATUS_NOT_INITIALIZED;
}

static bool heap_lock_exit(struct spinlock_irq_token *token)
{
    if (spinlock_irqrestore_release(&heap_lock, token) !=
            SPINLOCK_STATUS_OK) {
        heap_poisoned = true;
        return false;
    }
    return true;
}

static enum heap_status heap_lock_finish(
    struct spinlock_irq_token *token,
    enum heap_status status
)
{
    if (!heap_lock_exit(token)) {
        return HEAP_STATUS_CORRUPTED_STATE;
    }
    return status;
}

enum heap_status heap_initialize(void)
{
    struct spinlock_irq_token token;

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!heap_lock_register() || !heap_lock_acquire(&token)) {
        return HEAP_STATUS_CORRUPTED_STATE;
    }

    return heap_lock_finish(&token, heap_initialize_unguarded());
}

enum heap_status heap_allocate(
    size_t byte_size,
    size_t alignment,
    void **allocation
)
{
    struct spinlock_irq_token token;
    enum heap_status status;

    if (forbidden_context()) {
        if (allocation != NULL) {
            *allocation = NULL;
        }
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!heap_lock_acquire(&token)) {
        if (allocation != NULL) {
            *allocation = NULL;
        }
        return heap_lock_failure_status();
    }

    status = heap_allocate_unguarded(byte_size, alignment, allocation);
    if (!heap_lock_exit(&token)) {
        if (allocation != NULL) {
            *allocation = NULL;
        }
        return HEAP_STATUS_CORRUPTED_STATE;
    }
    if (status != HEAP_STATUS_OK && allocation != NULL) {
        *allocation = NULL;
    }
    return status;
}

enum heap_status heap_deallocate(void *allocation)
{
    struct spinlock_irq_token token;

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!heap_lock_acquire(&token)) {
        return heap_lock_failure_status();
    }

    return heap_lock_finish(&token, heap_deallocate_unguarded(allocation));
}

enum heap_status heap_validate(void)
{
    struct spinlock_irq_token token;

    if (forbidden_context()) {
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!heap_lock_acquire(&token)) {
        return heap_lock_failure_status();
    }

    return heap_lock_finish(&token, heap_validate_unguarded());
}

enum heap_status heap_get_stats(struct heap_stats *stats)
{
    struct spinlock_irq_token token;
    enum heap_status status;

    if (forbidden_context()) {
        if (stats != NULL) {
            struct heap_stats zero = {0U};

            *stats = zero;
        }
        return HEAP_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!heap_lock_acquire(&token)) {
        if (stats != NULL) {
            struct heap_stats zero = {0U};

            *stats = zero;
        }
        return heap_lock_failure_status();
    }
    status = heap_get_stats_unguarded(stats);
    if (!heap_lock_exit(&token)) {
        if (stats != NULL) {
            struct heap_stats zero = {0U};

            *stats = zero;
        }
        return HEAP_STATUS_CORRUPTED_STATE;
    }
    if (status != HEAP_STATUS_OK && stats != NULL) {
        struct heap_stats zero = {0U};

        *stats = zero;
    }
    return status;
}

const char *heap_status_string(enum heap_status status)
{
    switch (status) {
    case HEAP_STATUS_OK:
        return "ok";
    case HEAP_STATUS_NULL_ARGUMENT:
        return "null heap argument";
    case HEAP_STATUS_ALREADY_INITIALIZED:
        return "kernel heap is already initialized";
    case HEAP_STATUS_NOT_INITIALIZED:
        return "kernel heap is not initialized";
    case HEAP_STATUS_FORBIDDEN_CONTEXT:
        return "heap use is forbidden in interrupt or panic context";
    case HEAP_STATUS_ZERO_SIZE:
        return "heap allocation size is zero";
    case HEAP_STATUS_INVALID_ALIGNMENT:
        return "heap allocation alignment is invalid";
    case HEAP_STATUS_ARITHMETIC_OVERFLOW:
        return "heap arithmetic overflows";
    case HEAP_STATUS_WINDOW_EXHAUSTED:
        return "kernel heap window is exhausted";
    case HEAP_STATUS_DESCRIPTOR_LIMIT:
        return "kernel heap descriptor limit is exhausted";
    case HEAP_STATUS_OUT_OF_MEMORY:
        return "physical memory for heap growth is exhausted";
    case HEAP_STATUS_FRAME_FAILURE:
        return "physical-frame operation for heap growth failed";
    case HEAP_STATUS_MAPPING_FAILURE:
        return "heap page mapping operation failed";
    case HEAP_STATUS_ROLLBACK_FAILURE:
        return "heap growth rollback failed and the heap is poisoned";
    case HEAP_STATUS_POINTER_OUTSIDE_HEAP:
        return "pointer is outside committed heap memory";
    case HEAP_STATUS_MISALIGNED_POINTER:
        return "heap pointer is misaligned";
    case HEAP_STATUS_INTERIOR_POINTER:
        return "heap pointer is interior to a live allocation";
    case HEAP_STATUS_INVALID_POINTER:
        return "pointer has no live heap allocation provenance";
    case HEAP_STATUS_DOUBLE_FREE:
        return "heap allocation was freed twice";
    case HEAP_STATUS_CORRUPTED_STATE:
        return "kernel heap metadata is corrupted";
    case HEAP_STATUS_STATS_INVALID:
        return "kernel heap statistics are inconsistent";
    case HEAP_STATUS_VALIDATION_FAILURE:
        return "kernel heap validation failed";
    case HEAP_STATUS_TEST_FAILURE:
        return "injected heap growth failure";
    default:
        return "unknown kernel heap status";
    }
}

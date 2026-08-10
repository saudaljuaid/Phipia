/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/heap.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/pic.h>
#include <zenith/pit.h>
#include <zenith/scheduler.h>
#include <zenith/test.h>
#include <zenith/time.h>
#include <zenith/virtual_memory.h>

#define QEMU_EXIT_PORT UINT16_C(0x00F4)
#define QEMU_FAILURE_VALUE UINT8_C(0x7F)
#define PAGE_FAULT_TEST_ADDRESS UINT64_C(0x0000000100000000)
#define PAGE_FAULT_PRESENT UINT64_C(1)
#define PAGE_FAULT_WRITE UINT64_C(2)
#define PAGE_FAULT_INSTRUCTION UINT64_C(16)
#define PIT_TEST_FREQUENCY UINT32_C(100)
#define PIT_TEST_TICKS UINT64_C(8)
#define HEAP_TEST_ALLOCATION_COUNT 10U
#define HEAP_CONTEXT_TEST_VECTOR UINT8_C(0x81)
#define SCHEDULER_CONTEXT_TEST_VECTOR UINT8_C(0x82)
#define SCHEDULER_TEST_TASK_COUNT 3U
#define SCHEDULER_TEST_ROUNDS 4U
#define SCHEDULER_TEST_TRACE_LENGTH \
    (SCHEDULER_TEST_TASK_COUNT * SCHEDULER_TEST_ROUNDS)
#define SCHEDULER_TEST_STACK_PAGE_COUNT \
    VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES

volatile uint8_t kernel_test_double_fault_armed;
static enum kernel_test_scenario active_scenario;
static volatile bool heap_context_rejection_observed;
static volatile bool scheduler_context_rejection_observed;
static volatile uint32_t scheduler_trace[SCHEDULER_TEST_TRACE_LENGTH];
static volatile size_t scheduler_trace_count;
static volatile size_t scheduler_first_entry_count;
static volatile size_t scheduler_register_probe_count;
static volatile size_t scheduler_explicit_exit_count;
static uint64_t scheduler_guard_expected_address;

struct scheduler_test_context {
    uint32_t identifier;
};

static struct scheduler_test_context
    scheduler_test_contexts[SCHEDULER_TEST_TASK_COUNT];

static size_t literal_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static bool token_equals(const char *token, size_t token_length, const char *literal)
{
    const size_t expected_length = literal_length(literal);

    if (token_length != expected_length) {
        return false;
    }

    for (size_t index = 0; index < token_length; ++index) {
        if (token[index] != literal[index]) {
            return false;
        }
    }

    return true;
}

static bool token_has_prefix(
    const char *token,
    size_t token_length,
    const char *prefix,
    size_t *value_offset
)
{
    const size_t prefix_length = literal_length(prefix);

    if (token_length < prefix_length) {
        return false;
    }

    for (size_t index = 0; index < prefix_length; ++index) {
        if (token[index] != prefix[index]) {
            return false;
        }
    }

    *value_offset = prefix_length;
    return true;
}

static enum kernel_test_scenario scenario_from_value(
    const char *value,
    size_t length
)
{
    if (token_equals(value, length, "normal")) {
        return KERNEL_TEST_NORMAL;
    }

    if (token_equals(value, length, "breakpoint")) {
        return KERNEL_TEST_BREAKPOINT;
    }

    if (token_equals(value, length, "invalid-opcode")) {
        return KERNEL_TEST_INVALID_OPCODE;
    }

    if (token_equals(value, length, "page-fault")) {
        return KERNEL_TEST_PAGE_FAULT;
    }

    if (token_equals(value, length, "ist")) {
        return KERNEL_TEST_IST;
    }

    if (token_equals(value, length, "pit")) {
        return KERNEL_TEST_PIT;
    }

    if (token_equals(value, length, "unexpected")) {
        return KERNEL_TEST_UNEXPECTED;
    }

    if (token_equals(value, length, "double-fault")) {
        return KERNEL_TEST_DOUBLE_FAULT;
    }

    if (token_equals(value, length, "write-protect")) {
        return KERNEL_TEST_WRITE_PROTECT;
    }

    if (token_equals(value, length, "nx")) {
        return KERNEL_TEST_NX;
    }

    if (token_equals(value, length, "apic")) {
        return KERNEL_TEST_APIC;
    }

    if (token_equals(value, length, "lapic-timer")) {
        return KERNEL_TEST_LAPIC_TIMER;
    }

    if (token_equals(value, length, "heap")) {
        return KERNEL_TEST_HEAP;
    }

    if (token_equals(value, length, "scheduler")) {
        return KERNEL_TEST_SCHEDULER;
    }

    if (token_equals(value, length, "scheduler-guard")) {
        return KERNEL_TEST_SCHEDULER_GUARD;
    }

    return KERNEL_TEST_INVALID;
}

static uint8_t scenario_exit_value(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return UINT8_C(0x10);
    case KERNEL_TEST_BREAKPOINT:
        return UINT8_C(0x11);
    case KERNEL_TEST_INVALID_OPCODE:
        return UINT8_C(0x12);
    case KERNEL_TEST_PAGE_FAULT:
        return UINT8_C(0x13);
    case KERNEL_TEST_IST:
        return UINT8_C(0x14);
    case KERNEL_TEST_PIT:
        return UINT8_C(0x15);
    case KERNEL_TEST_UNEXPECTED:
        return UINT8_C(0x16);
    case KERNEL_TEST_DOUBLE_FAULT:
        return UINT8_C(0x17);
    case KERNEL_TEST_WRITE_PROTECT:
        return UINT8_C(0x18);
    case KERNEL_TEST_NX:
        return UINT8_C(0x19);
    case KERNEL_TEST_APIC:
        return UINT8_C(0x1A);
    case KERNEL_TEST_LAPIC_TIMER:
        return UINT8_C(0x1B);
    case KERNEL_TEST_HEAP:
        return UINT8_C(0x1C);
    case KERNEL_TEST_SCHEDULER:
        return UINT8_C(0x1D);
    case KERNEL_TEST_SCHEDULER_GUARD:
        return UINT8_C(0x1E);
    default:
        return QEMU_FAILURE_VALUE;
    }
}

static void test_marker(const char *kind, enum kernel_test_scenario scenario)
{
    console_write("ZT ");
    console_write(kind);
    console_putc(' ');
    console_write(kernel_test_scenario_name(scenario));
    console_putc('\n');
}

static void apic_timer_proof(void)
{
    const uint64_t eoi_before = apic_eoi_count();
    enum pit_status pit_status;

    pit_status = pit_start(PIT_TEST_FREQUENCY);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("APIC-routed PIT delivered too few ticks");
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (apic_eoi_count() < eoi_before + PIT_TEST_TICKS ||
        !apic_timer_is_masked() || !pic_all_masked() ||
        apic_validate() != APIC_STATUS_OK) {
        kernel_test_fail("APIC timer delivery contract failed validation");
    }
}

static _Noreturn void kernel_test_pass(void)
{
    const uint8_t exit_value = scenario_exit_value(active_scenario);

    test_marker("PASS", active_scenario);
    cpu_out32(QEMU_EXIT_PORT, exit_value);
    console_halt();
}

static bool ranges_overlap(
    uintptr_t left,
    size_t left_size,
    uintptr_t right,
    size_t right_size
)
{
    return left < right + right_size && right < left + left_size;
}

static void fill_pattern(void *allocation, size_t size, uint8_t seed)
{
    volatile uint8_t *bytes = (volatile uint8_t *)allocation;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = (uint8_t)(seed ^ (uint8_t)index);
    }
}

static bool pattern_matches(const void *allocation, size_t size, uint8_t seed)
{
    const volatile uint8_t *bytes =
        (const volatile uint8_t *)allocation;

    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != (uint8_t)(seed ^ (uint8_t)index)) {
            return false;
        }
    }

    return true;
}

static void expect_heap_status(
    enum heap_status observed,
    enum heap_status expected,
    const char *reason
)
{
    if (observed != expected) {
        kernel_test_fail(reason);
    }
}

static void heap_context_test_handler(
    struct interrupt_frame *frame,
    void *context
)
{
    (void)context;

    heap_context_rejection_observed = frame != NULL &&
        frame->vector == HEAP_CONTEXT_TEST_VECTOR &&
        heap_validate() == HEAP_STATUS_FORBIDDEN_CONTEXT;
}

static void heap_runtime_mapping_rejection_proof(void)
{
    const struct frame_allocator_stats before = frame_allocator_get_stats();
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct frame_allocator_stats after;
    struct virtual_memory_mapping mapping;
    uintptr_t frame;
    bool allocated;
    enum frame_owner owner;

    if (frame_allocate(&frame) != FRAME_STATUS_OK ||
        frame_query_allocation(frame, &allocated) != FRAME_STATUS_OK ||
        !allocated ||
        frame_query_owner(frame, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_GENERIC) {
        kernel_test_fail("runtime mapping proof could not allocate a frame");
    }

    cpu_interrupt_disable();

    if (virtual_memory_unmap_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        virtual_memory_map_heap_page(
            VIRTUAL_MEMORY_HEAP_BASE + 1U,
            frame
        ) != VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT ||
        virtual_memory_map_heap_page(
            UINT64_C(0x0000800000000000),
            frame
        ) != VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS ||
        virtual_memory_query(
            UINT64_C(0x0000800000000000),
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS ||
        virtual_memory_map_heap_page(
            VIRTUAL_MEMORY_HEAP_GUARD_BELOW,
            frame
        ) != VIRTUAL_MEMORY_STATUS_OUTSIDE_HEAP_WINDOW ||
        virtual_memory_map_heap_page(
            VIRTUAL_MEMORY_HEAP_BASE,
            (uintptr_t)UINT64_C(0x0010000000000000)
        ) != VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE ||
        virtual_memory_map_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_map_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) !=
            VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT ||
        virtual_memory_unmap_heap_page(
            VIRTUAL_MEMORY_HEAP_BASE,
            frame + (uintptr_t)ZENITH_PAGE_SIZE
        ) != VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH ||
        virtual_memory_unmap_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_unmap_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        kernel_test_fail("runtime mapping rejection contract failed");
    }

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    if (frame_release(frame) != FRAME_STATUS_OK ||
        frame_query_allocation(frame, &allocated) != FRAME_STATUS_OK ||
        allocated ||
        frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK) {
        kernel_test_fail("runtime mapping proof rollback failed");
    }

    after = frame_allocator_get_stats();

    if (after.allocated_frames != before.allocated_frames ||
        after.free_frames != before.free_frames ||
        after.generic_allocated_frames != before.generic_allocated_frames ||
        after.heap_allocated_frames != before.heap_allocated_frames) {
        kernel_test_fail("runtime mapping proof leaked a frame");
    }
}

static void heap_rollback_proof(void)
{
    const struct frame_allocator_stats frames_before =
        frame_allocator_get_stats();
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct frame_allocator_stats frames_after;
    struct heap_stats before;
    struct heap_stats after;
    void *allocation;

    expect_heap_status(
        heap_get_stats(&before),
        HEAP_STATUS_OK,
        "initial heap statistics are invalid"
    );

    for (size_t failure_index = 0U; failure_index < 3U; ++failure_index) {
        allocation = (void *)(uintptr_t)UINT64_C(1);

        if (!frame_test_fail_allocate_after(failure_index)) {
            kernel_test_fail(
                "could not arm frame-allocation failure injection"
            );
        }

        expect_heap_status(
            heap_allocate(
                2U * (size_t)ZENITH_PAGE_SIZE + 1U,
                4096U,
                &allocation
            ),
            HEAP_STATUS_TEST_FAILURE,
            "frame-allocation failure did not propagate"
        );

        if (allocation != NULL || heap_validate() != HEAP_STATUS_OK ||
            heap_get_stats(&after) != HEAP_STATUS_OK ||
            after.committed_bytes != before.committed_bytes ||
            after.mapped_pages != before.mapped_pages ||
            after.live_allocations != before.live_allocations) {
            kernel_test_fail("frame failure rollback changed heap ownership");
        }

        frames_after = frame_allocator_get_stats();

        if (frames_after.allocated_frames !=
                frames_before.allocated_frames ||
            frames_after.free_frames != frames_before.free_frames ||
            frames_after.generic_allocated_frames !=
                frames_before.generic_allocated_frames ||
            frames_after.heap_allocated_frames !=
                frames_before.heap_allocated_frames) {
            kernel_test_fail("frame failure rollback leaked a frame");
        }

        allocation = (void *)(uintptr_t)UINT64_C(1);
        cpu_interrupt_disable();

        if (!virtual_memory_test_fail_heap_map_after(failure_index)) {
            if (interrupts_were_enabled) {
                cpu_interrupt_enable();
            }

            kernel_test_fail("could not arm mapping failure injection");
        }

        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        expect_heap_status(
            heap_allocate(
                2U * (size_t)ZENITH_PAGE_SIZE + 1U,
                4096U,
                &allocation
            ),
            HEAP_STATUS_TEST_FAILURE,
            "mapping failure did not propagate"
        );

        if (allocation != NULL || heap_validate() != HEAP_STATUS_OK ||
            heap_get_stats(&after) != HEAP_STATUS_OK ||
            after.committed_bytes != before.committed_bytes ||
            after.mapped_pages != before.mapped_pages ||
            after.live_allocations != before.live_allocations) {
            kernel_test_fail("mapping failure rollback changed heap ownership");
        }

        frames_after = frame_allocator_get_stats();

        if (frames_after.allocated_frames !=
                frames_before.allocated_frames ||
            frames_after.free_frames != frames_before.free_frames ||
            frames_after.generic_allocated_frames !=
                frames_before.generic_allocated_frames ||
            frames_after.heap_allocated_frames !=
                frames_before.heap_allocated_frames) {
            kernel_test_fail("mapping failure rollback leaked a frame");
        }

        if (!interrupts_were_enabled) {
            cpu_interrupt_disable();
        }
    }

    allocation = (void *)(uintptr_t)UINT64_C(1);
    cpu_interrupt_disable();

    if (!virtual_memory_test_report_uncertain_heap_map_once()) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        kernel_test_fail("could not arm uncertain mapping failure injection");
    }

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    expect_heap_status(
        heap_allocate(
            2U * (size_t)ZENITH_PAGE_SIZE + 1U,
            4096U,
            &allocation
        ),
        HEAP_STATUS_MAPPING_FAILURE,
        "uncertain mapping failure did not propagate"
    );

    if (allocation != NULL || heap_validate() != HEAP_STATUS_OK ||
        heap_get_stats(&after) != HEAP_STATUS_OK ||
        after.committed_bytes != before.committed_bytes ||
        after.mapped_pages != before.mapped_pages ||
        after.live_allocations != before.live_allocations) {
        kernel_test_fail("uncertain mapping cleanup changed heap ownership");
    }

    frames_after = frame_allocator_get_stats();

    if (frames_after.allocated_frames != frames_before.allocated_frames ||
        frames_after.free_frames != frames_before.free_frames ||
        frames_after.generic_allocated_frames !=
            frames_before.generic_allocated_frames ||
        frames_after.heap_allocated_frames !=
            frames_before.heap_allocated_frames) {
        kernel_test_fail("uncertain mapping cleanup leaked a frame");
    }

    if (!interrupts_were_enabled) {
        cpu_interrupt_disable();
    }
}

static void heap_basic_allocation_proof(void)
{
    static const size_t sizes[HEAP_TEST_ALLOCATION_COUNT] = {
        257U, 4096U, 5000U, 31U, 63U, 127U, 255U, 511U, 1023U, 2047U
    };
    static const size_t alignments[HEAP_TEST_ALLOCATION_COUNT] = {
        16U, 4096U, 64U, 1U, 2U, 32U, 128U, 256U, 1024U, 2048U
    };
    void *allocations[HEAP_TEST_ALLOCATION_COUNT];

    for (size_t index = 0U; index < HEAP_TEST_ALLOCATION_COUNT; ++index) {
        allocations[index] = NULL;
        expect_heap_status(
            heap_allocate(sizes[index], alignments[index], &allocations[index]),
            HEAP_STATUS_OK,
            "basic heap allocation failed"
        );

        if (allocations[index] == NULL ||
            ((uintptr_t)allocations[index] & (alignments[index] - 1U)) != 0U) {
            kernel_test_fail("heap allocation did not honor its alignment");
        }

        for (size_t other = 0U; other < index; ++other) {
            if (ranges_overlap(
                    (uintptr_t)allocations[index],
                    sizes[index],
                    (uintptr_t)allocations[other],
                    sizes[other]
                )) {
                kernel_test_fail("live heap allocations overlap");
            }
        }

        fill_pattern(allocations[index], sizes[index], (uint8_t)(0x31U + index));
    }

    for (size_t index = 0U; index < HEAP_TEST_ALLOCATION_COUNT; ++index) {
        if (!pattern_matches(
                allocations[index],
                sizes[index],
                (uint8_t)(0x31U + index)
            )) {
            kernel_test_fail("heap payload pattern did not survive");
        }
    }

    for (size_t count = HEAP_TEST_ALLOCATION_COUNT; count > 0U; --count) {
        expect_heap_status(
            heap_deallocate(allocations[count - 1U]),
            HEAP_STATUS_OK,
            "basic heap deallocation failed"
        );
    }
}

static void heap_reuse_and_rejection_proof(void)
{
    void *first;
    void *second;
    void *third;
    void *combined;
    void *reused;

    expect_heap_status(
        heap_allocate(512U, 16U, &first),
        HEAP_STATUS_OK,
        "first coalescing allocation failed"
    );
    expect_heap_status(
        heap_allocate(512U, 16U, &second),
        HEAP_STATUS_OK,
        "second coalescing allocation failed"
    );
    expect_heap_status(
        heap_allocate(512U, 16U, &third),
        HEAP_STATUS_OK,
        "third coalescing allocation failed"
    );

    if ((uintptr_t)second != (uintptr_t)first + 512U ||
        (uintptr_t)third != (uintptr_t)second + 512U) {
        kernel_test_fail("first-fit split order is not deterministic");
    }

    expect_heap_status(heap_deallocate(first), HEAP_STATUS_OK,
        "forward coalescing setup failed");
    expect_heap_status(heap_deallocate(third), HEAP_STATUS_OK,
        "backward coalescing setup failed");
    expect_heap_status(heap_deallocate(second), HEAP_STATUS_OK,
        "bidirectional coalescing failed");
    expect_heap_status(
        heap_allocate(1536U, 16U, &combined),
        HEAP_STATUS_OK,
        "coalesced allocation could not be reused"
    );

    if (combined != first) {
        kernel_test_fail("coalescing did not recreate the larger first block");
    }

    expect_heap_status(heap_deallocate(combined), HEAP_STATUS_OK,
        "coalesced allocation deallocation failed");
    expect_heap_status(heap_allocate(128U, 16U, &first), HEAP_STATUS_OK,
        "reuse setup allocation failed");
    expect_heap_status(
        heap_deallocate((void *)((uintptr_t)first + 16U)),
        HEAP_STATUS_INTERIOR_POINTER,
        "interior heap pointer was not rejected"
    );
    expect_heap_status(
        heap_deallocate((void *)((uintptr_t)first + 1U)),
        HEAP_STATUS_MISALIGNED_POINTER,
        "misaligned heap pointer was not rejected"
    );
    expect_heap_status(
        heap_deallocate((void *)(uintptr_t)(VIRTUAL_MEMORY_HEAP_BASE - 16U)),
        HEAP_STATUS_POINTER_OUTSIDE_HEAP,
        "outside heap pointer was not rejected"
    );
    expect_heap_status(heap_deallocate(first), HEAP_STATUS_OK,
        "reuse setup deallocation failed");
    expect_heap_status(heap_deallocate(first), HEAP_STATUS_DOUBLE_FREE,
        "heap double free was not rejected"
    );
    expect_heap_status(heap_allocate(128U, 16U, &reused), HEAP_STATUS_OK,
        "freed allocation could not be reused");

    if (reused != first) {
        kernel_test_fail("first-fit allocator did not reuse freed space");
    }

    expect_heap_status(heap_deallocate(reused), HEAP_STATUS_OK,
        "reused allocation deallocation failed");
    expect_heap_status(heap_allocate(0U, 16U, &reused), HEAP_STATUS_ZERO_SIZE,
        "zero-sized allocation was not rejected");
    expect_heap_status(heap_allocate(1U, 3U, &reused),
        HEAP_STATUS_INVALID_ALIGNMENT,
        "non-power-of-two alignment was not rejected");
    expect_heap_status(heap_allocate(1U, 8192U, &reused),
        HEAP_STATUS_INVALID_ALIGNMENT,
        "excessive alignment was not rejected");
}

static void heap_exhaustion_and_mapping_proof(void)
{
    const struct frame_allocator_stats frames_before =
        frame_allocator_get_stats();
    struct frame_allocator_stats frames_after;
    struct heap_stats stats_before;
    struct heap_stats stats;
    struct virtual_memory_mapping mapping;
    void *whole_window;
    void *failed;

    expect_heap_status(heap_get_stats(&stats_before), HEAP_STATUS_OK,
        "pre-exhaustion heap statistics are invalid");

    expect_heap_status(
        heap_allocate(
            (size_t)VIRTUAL_MEMORY_HEAP_SIZE,
            HEAP_MAXIMUM_ALIGNMENT,
            &whole_window
        ),
        HEAP_STATUS_OK,
        "largest valid heap request failed"
    );

    if ((uintptr_t)whole_window != VIRTUAL_MEMORY_HEAP_BASE) {
        kernel_test_fail("largest request did not use the whole heap window");
    }

    failed = (void *)(uintptr_t)UINT64_C(1);
    expect_heap_status(
        heap_allocate(1U, 1U, &failed),
        HEAP_STATUS_WINDOW_EXHAUSTED,
        "first request beyond the heap window was not rejected"
    );

    if (failed != NULL) {
        kernel_test_fail("failed heap allocation published a pointer");
    }

    ((volatile uint8_t *)whole_window)[0] = UINT8_C(0xA5);
    ((volatile uint8_t *)whole_window)[VIRTUAL_MEMORY_HEAP_SIZE / 2U] =
        UINT8_C(0x5A);
    ((volatile uint8_t *)whole_window)[VIRTUAL_MEMORY_HEAP_SIZE - 1U] =
        UINT8_C(0xC3);

    if (((volatile uint8_t *)whole_window)[0] != UINT8_C(0xA5) ||
        ((volatile uint8_t *)whole_window)[VIRTUAL_MEMORY_HEAP_SIZE / 2U] !=
            UINT8_C(0x5A) ||
        ((volatile uint8_t *)whole_window)[VIRTUAL_MEMORY_HEAP_SIZE - 1U] !=
            UINT8_C(0xC3) ||
        heap_validate() != HEAP_STATUS_OK) {
        kernel_test_fail("exhausted heap was corrupted");
    }

    if (virtual_memory_query(VIRTUAL_MEMORY_HEAP_BASE, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        virtual_memory_query(
            VIRTUAL_MEMORY_HEAP_GUARD_ABOVE - ZENITH_PAGE_SIZE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        virtual_memory_query(VIRTUAL_MEMORY_HEAP_GUARD_BELOW, &mapping) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        virtual_memory_query(VIRTUAL_MEMORY_HEAP_GUARD_ABOVE, &mapping) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        kernel_test_fail("heap page or guard permissions are invalid");
    }

    frames_after = frame_allocator_get_stats();

    if (frames_after.allocated_frames !=
            frames_before.allocated_frames +
            (size_t)(VIRTUAL_MEMORY_HEAP_SIZE / ZENITH_PAGE_SIZE) -
                stats_before.mapped_pages ||
        frames_after.generic_allocated_frames !=
            frames_before.generic_allocated_frames ||
        frames_after.heap_allocated_frames !=
            frames_before.heap_allocated_frames +
                (size_t)(VIRTUAL_MEMORY_HEAP_SIZE / ZENITH_PAGE_SIZE) -
                    stats_before.mapped_pages) {
        kernel_test_fail("heap backing-frame statistics are inconsistent");
    }

    expect_heap_status(heap_deallocate(whole_window), HEAP_STATUS_OK,
        "whole-window allocation deallocation failed");
    expect_heap_status(heap_get_stats(&stats), HEAP_STATUS_OK,
        "final heap statistics are invalid");

    if (stats.committed_bytes != (size_t)VIRTUAL_MEMORY_HEAP_SIZE ||
        stats.mapped_pages !=
            (size_t)(VIRTUAL_MEMORY_HEAP_SIZE / ZENITH_PAGE_SIZE) ||
        stats.live_allocations != 0U ||
        stats.live_requested_bytes != 0U ||
        stats.live_extent_bytes != 0U ||
        stats.reusable_bytes != (size_t)VIRTUAL_MEMORY_HEAP_SIZE ||
        stats.largest_reusable_block !=
            (size_t)VIRTUAL_MEMORY_HEAP_SIZE ||
        stats.descriptors_in_use != 1U) {
        kernel_test_fail("fully freed heap statistics are impossible");
    }
}

static void heap_interrupt_state_proof(void)
{
    void *allocation;

    if (!cpu_interrupts_enabled()) {
        kernel_test_fail("heap scenario expected interrupts to be enabled");
    }

    cpu_interrupt_disable();
    expect_heap_status(
        heap_allocate(64U, 64U, &allocation),
        HEAP_STATUS_OK,
        "interrupt-disabled heap allocation failed"
    );

    if (cpu_interrupts_enabled()) {
        kernel_test_fail("heap allocation changed a disabled IF state");
    }

    expect_heap_status(
        heap_deallocate(allocation),
        HEAP_STATUS_OK,
        "interrupt-disabled heap deallocation failed"
    );

    if (cpu_interrupts_enabled()) {
        kernel_test_fail("heap deallocation changed a disabled IF state");
    }

    cpu_interrupt_enable();
}

static void expect_scheduler_status(
    enum scheduler_status observed,
    enum scheduler_status expected,
    const char *reason
)
{
    if (observed != expected) {
        kernel_test_fail(reason);
    }
}

static void scheduler_noop_task(void *context)
{
    (void)context;
}

static void scheduler_context_test_handler(
    struct interrupt_frame *frame,
    void *context
)
{
    (void)context;
    scheduler_context_rejection_observed = frame != NULL &&
        frame->vector == SCHEDULER_CONTEXT_TEST_VECTOR &&
        scheduler_validate() == SCHEDULER_STATUS_FORBIDDEN_CONTEXT &&
        scheduler_yield() == SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
}

static void scheduler_worker_task(void *opaque_context)
{
    struct scheduler_test_context *context =
        (struct scheduler_test_context *)opaque_context;
    volatile uint8_t stack_pattern[1024];
    void *heap_allocation = NULL;
    uint64_t last_tick;

    if (context == NULL ||
        context->identifier >= SCHEDULER_TEST_TASK_COUNT ||
        !cpu_interrupts_enabled() ||
        (cpu_read_stack_pointer() & (uintptr_t)0xFU) != (uintptr_t)8U) {
        kernel_test_fail("scheduler task first-entry ABI is invalid");
    }

    ++scheduler_first_entry_count;

    for (size_t index = 0U; index < sizeof(stack_pattern); ++index) {
        stack_pattern[index] = (uint8_t)(
            UINT8_C(0x5A) ^ (uint8_t)context->identifier ^ (uint8_t)index
        );
    }

    if (context->identifier == 0U) {
        expect_heap_status(
            heap_allocate(8193U, 64U, &heap_allocation),
            HEAP_STATUS_OK,
            "scheduler heap task could not allocate"
        );
        fill_pattern(heap_allocation, 8193U, UINT8_C(0xC7));
    }

    if (!scheduler_test_callee_saved_probe()) {
        kernel_test_fail("scheduler lost a SysV callee-saved register");
    }

    ++scheduler_register_probe_count;
    last_tick = kernel_time_monotonic_ticks();

    for (size_t round = 0U; round < SCHEDULER_TEST_ROUNDS; ++round) {
        bool timer_advanced = false;
        size_t position = scheduler_trace_count;

        if (position >= SCHEDULER_TEST_TRACE_LENGTH) {
            kernel_test_fail("scheduler round-robin trace overflowed");
        }

        scheduler_trace[position] = context->identifier;
        scheduler_trace_count = position + 1U;

        for (size_t index = 0U; index < sizeof(stack_pattern); ++index) {
            if (stack_pattern[index] != (uint8_t)(
                    UINT8_C(0x5A) ^ (uint8_t)context->identifier ^
                        (uint8_t)index
                )) {
                kernel_test_fail("task-local stack pattern was corrupted");
            }
        }

        if (heap_allocation != NULL &&
            (!pattern_matches(heap_allocation, 8193U, UINT8_C(0xC7)) ||
             heap_validate() != HEAP_STATUS_OK)) {
            kernel_test_fail("live heap allocation changed across a yield");
        }

        for (uint64_t attempt = 0U;
             attempt < UINT64_C(50000000);
             ++attempt) {
            if (kernel_time_monotonic_ticks() > last_tick) {
                timer_advanced = true;
                last_tick = kernel_time_monotonic_ticks();
                break;
            }

            __asm__ volatile ("pause" : : : "memory");
        }

        if (!timer_advanced) {
            kernel_test_fail("Local APIC time stopped on a task stack");
        }

        expect_scheduler_status(
            scheduler_yield(),
            SCHEDULER_STATUS_OK,
            "cooperative task yield failed"
        );
    }

    if (heap_allocation != NULL) {
        expect_heap_status(
            heap_deallocate(heap_allocation),
            HEAP_STATUS_OK,
            "scheduler heap task could not free"
        );

        if (heap_validate() != HEAP_STATUS_OK) {
            kernel_test_fail("scheduler heap task left invalid heap state");
        }
    }

    if (context->identifier == SCHEDULER_TEST_TASK_COUNT - 1U) {
        ++scheduler_explicit_exit_count;
        scheduler_task_exit();
    }
}

static void scheduler_failure_rollback_proof(void)
{
    struct scheduler_task_handle handle;

    for (size_t failure = 0U;
         failure < SCHEDULER_TEST_STACK_PAGE_COUNT;
         ++failure) {
        handle.index = 0U;
        handle.generation = 1U;

        if (!frame_test_fail_allocate_after(failure)) {
            kernel_test_fail("could not arm task-stack frame failure");
        }

        expect_scheduler_status(
            scheduler_task_create(scheduler_noop_task, NULL, &handle),
            SCHEDULER_STATUS_TEST_FAILURE,
            "task-stack frame failure did not roll back"
        );

        if (handle.index != SCHEDULER_INVALID_INDEX ||
            handle.generation != 0U ||
            scheduler_validate() != SCHEDULER_STATUS_OK) {
            kernel_test_fail("frame rollback published scheduler state");
        }

        cpu_interrupt_disable();

        if (!virtual_memory_test_fail_task_stack_map_after(failure)) {
            kernel_test_fail("could not arm task-stack mapping failure");
        }

        expect_scheduler_status(
            scheduler_task_create(scheduler_noop_task, NULL, &handle),
            SCHEDULER_STATUS_TEST_FAILURE,
            "task-stack mapping failure did not roll back"
        );

        if (handle.index != SCHEDULER_INVALID_INDEX ||
            handle.generation != 0U ||
            scheduler_validate() != SCHEDULER_STATUS_OK) {
            kernel_test_fail("mapping rollback published scheduler state");
        }
    }

    cpu_interrupt_disable();

    if (!virtual_memory_test_report_uncertain_task_stack_map_once()) {
        kernel_test_fail("could not arm uncertain task-stack mapping result");
    }

    expect_scheduler_status(
        scheduler_task_create(scheduler_noop_task, NULL, &handle),
        SCHEDULER_STATUS_MAPPING_FAILURE,
        "uncertain task-stack mapping result was not cleaned"
    );

    if (!frame_test_report_out_of_memory_after(0U)) {
        kernel_test_fail("could not arm task-stack physical OOM");
    }

    expect_scheduler_status(
        scheduler_task_create(scheduler_noop_task, NULL, &handle),
        SCHEDULER_STATUS_OUT_OF_MEMORY,
        "task-stack physical OOM did not propagate"
    );

    if (scheduler_validate() != SCHEDULER_STATUS_OK) {
        kernel_test_fail("scheduler rollback proof leaked ownership");
    }
}

static void scheduler_descriptor_exhaustion_proof(void)
{
    struct scheduler_task_handle handles[SCHEDULER_TASK_LIMIT];
    struct scheduler_task_handle failed;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        expect_scheduler_status(
            scheduler_task_create(
                scheduler_noop_task,
                NULL,
                &handles[index]
            ),
            SCHEDULER_STATUS_OK,
            "scheduler descriptor fill failed"
        );
    }

    expect_scheduler_status(
        scheduler_task_create(scheduler_noop_task, NULL, &failed),
        SCHEDULER_STATUS_DESCRIPTOR_LIMIT,
        "scheduler descriptor exhaustion was not deterministic"
    );

    expect_scheduler_status(
        scheduler_yield(),
        SCHEDULER_STATUS_OK,
        "descriptor-exhaustion tasks did not run"
    );

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        struct scheduler_task_info info;

        expect_scheduler_status(
            scheduler_task_query(handles[index], &info),
            SCHEDULER_STATUS_OK,
            "completed descriptor task was not queryable"
        );

        if (info.state != SCHEDULER_TASK_EXITED) {
            kernel_test_fail("completed descriptor task remained runnable");
        }

        expect_scheduler_status(
            scheduler_task_reap(handles[index]),
            SCHEDULER_STATUS_OK,
            "completed descriptor task could not be reaped"
        );
    }
}

static volatile unsigned int scheduler_normal_stage;

static void scheduler_normal_task(void *context)
{
    (void)context;

    if (!cpu_interrupts_enabled()) {
        kernel_test_fail("normal scheduler task began with IF clear");
    }

    scheduler_normal_stage = 1U;
    expect_scheduler_status(
        scheduler_yield(),
        SCHEDULER_STATUS_OK,
        "normal scheduler task could not yield"
    );
    scheduler_normal_stage = 2U;
}

void kernel_test_scheduler_normal_proof(void)
{
    struct scheduler_task_handle handle;
    struct scheduler_task_info info;

    scheduler_normal_stage = 0U;
    expect_scheduler_status(
        scheduler_task_create(scheduler_normal_task, NULL, &handle),
        SCHEDULER_STATUS_OK,
        "normal scheduler task creation failed"
    );
    expect_scheduler_status(
        scheduler_yield(),
        SCHEDULER_STATUS_OK,
        "normal scheduler first yield failed"
    );

    if (scheduler_normal_stage != 1U) {
        kernel_test_fail("normal scheduler task did not run once");
    }

    expect_scheduler_status(
        scheduler_yield(),
        SCHEDULER_STATUS_OK,
        "normal scheduler completion yield failed"
    );

    if (scheduler_normal_stage != 2U ||
        scheduler_task_query(handle, &info) != SCHEDULER_STATUS_OK ||
        info.state != SCHEDULER_TASK_EXITED ||
        scheduler_task_reap(handle) != SCHEDULER_STATUS_OK ||
        scheduler_yield() != SCHEDULER_STATUS_NO_RUNNABLE_PEER ||
        scheduler_validate() != SCHEDULER_STATUS_OK) {
        kernel_test_fail("normal scheduler lifecycle proof failed");
    }
}

enum kernel_test_scenario kernel_test_select(const struct boot_context *context)
{
    static const char prefix[] = "zenith.test=";
    enum kernel_test_scenario selected = KERNEL_TEST_NONE;
    size_t offset = 0;

    kernel_test_double_fault_armed = 0U;
    active_scenario = KERNEL_TEST_NONE;

    if (context == NULL || context->command_line == NULL) {
        return KERNEL_TEST_NONE;
    }

    while (offset < context->command_line_length) {
        size_t token_start;
        size_t token_length;
        size_t value_offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] == ' ') {
            ++offset;
        }

        token_start = offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] != ' ') {
            ++offset;
        }

        token_length = offset - token_start;

        if (token_length == 0U || !token_has_prefix(
                context->command_line + token_start,
                token_length,
                prefix,
                &value_offset
            )) {
            continue;
        }

        if (selected != KERNEL_TEST_NONE) {
            return KERNEL_TEST_INVALID;
        }

        selected = scenario_from_value(
            context->command_line + token_start + value_offset,
            token_length - value_offset
        );

        if (selected == KERNEL_TEST_INVALID) {
            return selected;
        }
    }

    active_scenario = selected;
    return selected;
}

void kernel_test_run(enum kernel_test_scenario scenario)
{
    if (scenario == KERNEL_TEST_NONE) {
        return;
    }

    active_scenario = scenario;
    test_marker("BEGIN", scenario);

    if (!interrupt_frame_layout_self_test()) {
        kernel_test_fail("interrupt frame or descriptor validation failed");
    }

    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return;
    case KERNEL_TEST_BREAKPOINT:
        if (!interrupt_breakpoint_self_test()) {
            kernel_test_fail("breakpoint register restoration failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_INVALID_OPCODE:
        interrupt_trigger_invalid_opcode();
    case KERNEL_TEST_PAGE_FAULT:
        interrupt_trigger_page_fault();
    case KERNEL_TEST_IST:
        if (!interrupt_ist_self_test()) {
            kernel_test_fail("IST routing proof failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_PIT:
        apic_timer_proof();
        kernel_test_pass();
    case KERNEL_TEST_UNEXPECTED:
        interrupt_trigger_unexpected();
    case KERNEL_TEST_DOUBLE_FAULT:
        kernel_test_double_fault_armed = 1U;
        interrupt_test_set_gate_present(14U, false);
        interrupt_trigger_page_fault();
    case KERNEL_TEST_WRITE_PROTECT:
        interrupt_trigger_write_protect();
    case KERNEL_TEST_NX:
        interrupt_trigger_nx();
    case KERNEL_TEST_APIC:
        if (!apic_spurious_self_test()) {
            kernel_test_fail("APIC spurious-vector proof failed");
        }

        apic_timer_proof();
        kernel_test_pass();
    case KERNEL_TEST_LAPIC_TIMER:
    case KERNEL_TEST_HEAP:
    case KERNEL_TEST_SCHEDULER:
    case KERNEL_TEST_SCHEDULER_GUARD:
        return;
    case KERNEL_TEST_INVALID:
        kernel_test_fail("invalid or duplicate zenith.test argument");
    case KERNEL_TEST_NONE:
    default:
        kernel_test_fail("unreachable test scenario");
    }
}

_Noreturn void kernel_test_complete_normal(void)
{
    if (active_scenario != KERNEL_TEST_NORMAL) {
        kernel_test_fail("normal completion used outside the normal scenario");
    }

    kernel_test_pass();
}

_Noreturn void kernel_test_complete_lapic_timer(void)
{
    if (active_scenario != KERNEL_TEST_LAPIC_TIMER) {
        kernel_test_fail(
            "Local APIC timer completion used outside its scenario"
        );
    }

    kernel_test_pass();
}

_Noreturn void kernel_test_complete_heap(void)
{
    uint64_t ticks_before;
    uint64_t ticks_after;
    uint64_t eoi_before;

    if (active_scenario != KERNEL_TEST_HEAP) {
        kernel_test_fail("heap completion used outside the heap scenario");
    }

    if (heap_initialize() != HEAP_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("heap initialization did not reject repetition");
    }

    if (cpu_interrupts_enabled()) {
        kernel_test_fail("heap scenario expected its caller to have IF clear");
    }

    heap_context_rejection_observed = false;

    if (interrupt_register_handler(
            HEAP_CONTEXT_TEST_VECTOR,
            heap_context_test_handler,
            NULL
        ) != INTERRUPT_STATUS_OK) {
        kernel_test_fail("could not register heap context rejection probe");
    }

    cpu_interrupt_enable();
    __asm__ volatile ("int $0x81" : : : "memory");

    if (!heap_context_rejection_observed) {
        kernel_test_fail("interrupt-context heap use was not rejected");
    }

    cpu_interrupt_disable();

    if (interrupt_unregister_handler(HEAP_CONTEXT_TEST_VECTOR) !=
        INTERRUPT_STATUS_OK) {
        kernel_test_fail("could not unregister heap context rejection probe");
    }

    cpu_interrupt_enable();

    ticks_before = kernel_time_monotonic_ticks();
    eoi_before = apic_eoi_count();
    heap_runtime_mapping_rejection_proof();
    heap_rollback_proof();
    heap_basic_allocation_proof();
    heap_reuse_and_rejection_proof();
    heap_exhaustion_and_mapping_proof();
    heap_interrupt_state_proof();

    if (!cpu_interrupts_enabled()) {
        kernel_test_fail("heap operations did not restore enabled IF state");
    }

    cpu_interrupt_disable();

    if (kernel_time_wait_for_ticks(UINT64_C(2)) != KERNEL_TIME_STATUS_OK) {
        kernel_test_fail("monotonic timer stopped during heap operations");
    }

    ticks_after = kernel_time_monotonic_ticks();

    if (ticks_after <= ticks_before || apic_eoi_count() <= eoi_before ||
        pit_is_running() || !apic_timer_is_masked() || !pic_all_masked() ||
        !apic_local_timer_active() ||
        kernel_time_validate() != KERNEL_TIME_STATUS_OK ||
        heap_validate() != HEAP_STATUS_OK) {
        kernel_test_fail("heap operations violated the timer or APIC contract");
    }

    console_write("Zenith OS: bounded kernel heap verified\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_scheduler(void)
{
    struct scheduler_task_handle handles[SCHEDULER_TEST_TASK_COUNT];
    struct scheduler_task_info infos[SCHEDULER_TEST_TASK_COUNT];
    struct scheduler_task_identity identity_before;
    struct scheduler_task_identity identity_after;
    struct scheduler_stats stats;
    struct heap_stats heap_stats;
    uint64_t ticks_before;
    uint64_t eoi_before;
    bool all_exited = false;

    if (active_scenario != KERNEL_TEST_SCHEDULER) {
        kernel_test_fail("scheduler completion used outside its scenario");
    }

    expect_scheduler_status(
        scheduler_initialize(),
        SCHEDULER_STATUS_ALREADY_INITIALIZED,
        "scheduler initialization did not reject repetition"
    );
    expect_scheduler_status(
        scheduler_task_create(scheduler_noop_task, NULL, NULL),
        SCHEDULER_STATUS_NULL_ARGUMENT,
        "scheduler accepted a null output handle"
    );
    expect_scheduler_status(
        scheduler_task_create(NULL, NULL, &handles[0]),
        SCHEDULER_STATUS_INVALID_ENTRY,
        "scheduler accepted a null entry function"
    );
    expect_scheduler_status(
        scheduler_task_reap((struct scheduler_task_handle){
            .index = SCHEDULER_INVALID_INDEX,
            .generation = 1U
        }),
        SCHEDULER_STATUS_INVALID_HANDLE,
        "scheduler accepted an out-of-range handle"
    );

    if (scheduler_current_task(&identity_before) != SCHEDULER_STATUS_OK ||
        !identity_before.bootstrap) {
        kernel_test_fail("bootstrap task was not explicitly represented");
    }

    scheduler_failure_rollback_proof();
    scheduler_context_rejection_observed = false;
    cpu_interrupt_disable();

    if (interrupt_register_handler(
            SCHEDULER_CONTEXT_TEST_VECTOR,
            scheduler_context_test_handler,
            NULL
        ) != INTERRUPT_STATUS_OK) {
        kernel_test_fail("could not register scheduler context probe");
    }

    cpu_interrupt_enable();
    __asm__ volatile ("int $0x82" : : : "memory");

    if (!scheduler_context_rejection_observed) {
        kernel_test_fail("interrupt-context scheduler use was not rejected");
    }

    cpu_interrupt_disable();

    if (interrupt_unregister_handler(SCHEDULER_CONTEXT_TEST_VECTOR) !=
            INTERRUPT_STATUS_OK) {
        kernel_test_fail("could not unregister scheduler context probe");
    }

    scheduler_trace_count = 0U;
    scheduler_first_entry_count = 0U;
    scheduler_register_probe_count = 0U;
    scheduler_explicit_exit_count = 0U;

    for (size_t index = 0U; index < SCHEDULER_TEST_TASK_COUNT; ++index) {
        scheduler_test_contexts[index].identifier = (uint32_t)index;
        expect_scheduler_status(
            scheduler_task_create(
                scheduler_worker_task,
                &scheduler_test_contexts[index],
                &handles[index]
            ),
            SCHEDULER_STATUS_OK,
            "scheduler worker creation failed"
        );
        expect_scheduler_status(
            scheduler_task_query(handles[index], &infos[index]),
            SCHEDULER_STATUS_OK,
            "scheduler worker query failed"
        );

        if (infos[index].state != SCHEDULER_TASK_READY ||
            infos[index].stack_end - infos[index].stack_start !=
                UINT64_C(65536) ||
            infos[index].lower_guard + ZENITH_PAGE_SIZE !=
                infos[index].stack_start ||
            infos[index].upper_guard != infos[index].stack_end) {
            kernel_test_fail("scheduler worker stack layout is invalid");
        }

        for (size_t page = 0U;
             page < SCHEDULER_TEST_STACK_PAGE_COUNT;
             ++page) {
            struct virtual_memory_mapping mapping;

            if (virtual_memory_query_task_stack_page(
                    handles[index].index,
                    page,
                    &mapping
                ) != VIRTUAL_MEMORY_STATUS_OK ||
                mapping.permissions != VIRTUAL_MEMORY_WRITABLE) {
                kernel_test_fail("task-stack leaf permissions are invalid");
            }
        }

        if (index != 0U &&
            infos[index - 1U].stack_end >= infos[index].stack_start) {
            kernel_test_fail("live task-stack payloads overlap");
        }
    }

    ticks_before = kernel_time_monotonic_ticks();
    eoi_before = apic_eoi_count();

    if (kernel_time_wait_for_ticks(UINT64_C(2)) != KERNEL_TIME_STATUS_OK ||
        scheduler_current_task(&identity_after) != SCHEDULER_STATUS_OK ||
        !identity_after.bootstrap || scheduler_trace_count != 0U ||
        kernel_time_monotonic_ticks() <= ticks_before ||
        apic_eoi_count() <= eoi_before) {
        kernel_test_fail("timer ticks incorrectly switched scheduler tasks");
    }

    for (size_t attempt = 0U; attempt < 32U; ++attempt) {
        all_exited = true;

        for (size_t index = 0U; index < SCHEDULER_TEST_TASK_COUNT; ++index) {
            if (scheduler_task_query(handles[index], &infos[index]) !=
                    SCHEDULER_STATUS_OK) {
                kernel_test_fail("scheduler worker state became unqueryable");
            }

            if (infos[index].state != SCHEDULER_TASK_EXITED) {
                all_exited = false;
            }
        }

        if (all_exited) {
            break;
        }

        expect_scheduler_status(
            scheduler_yield(),
            SCHEDULER_STATUS_OK,
            "bootstrap scheduler yield failed"
        );

        if (cpu_interrupts_enabled()) {
            kernel_test_fail("scheduler yield changed bootstrap IF state");
        }
    }

    if (!all_exited ||
        scheduler_trace_count != SCHEDULER_TEST_TRACE_LENGTH ||
        scheduler_first_entry_count != SCHEDULER_TEST_TASK_COUNT ||
        scheduler_register_probe_count != SCHEDULER_TEST_TASK_COUNT ||
        scheduler_explicit_exit_count != 1U) {
        kernel_test_fail("scheduler workers did not complete deterministically");
    }

    for (size_t position = 0U;
         position < SCHEDULER_TEST_TRACE_LENGTH;
         ++position) {
        if (scheduler_trace[position] !=
            (uint32_t)(position % SCHEDULER_TEST_TASK_COUNT)) {
            kernel_test_fail("round-robin scheduler trace is not deterministic");
        }
    }

    for (size_t index = 0U; index < SCHEDULER_TEST_TASK_COUNT; ++index) {
        expect_scheduler_status(
            scheduler_task_reap(handles[index]),
            SCHEDULER_STATUS_OK,
            "exited scheduler worker could not be reaped"
        );
        expect_scheduler_status(
            scheduler_task_reap(handles[index]),
            SCHEDULER_STATUS_STALE_HANDLE,
            "stale reaped task handle was not rejected"
        );
    }

    {
        struct scheduler_task_handle reused;

        expect_scheduler_status(
            scheduler_task_create(scheduler_noop_task, NULL, &reused),
            SCHEDULER_STATUS_OK,
            "reclaimed scheduler slot could not be reused"
        );

        if (reused.index != handles[0].index ||
            reused.generation != handles[0].generation + 1U) {
            kernel_test_fail("scheduler slot reuse did not advance generation");
        }

        expect_scheduler_status(
            scheduler_yield(),
            SCHEDULER_STATUS_OK,
            "reused scheduler task did not run"
        );
        expect_scheduler_status(
            scheduler_task_reap(reused),
            SCHEDULER_STATUS_OK,
            "reused scheduler task could not be reaped"
        );
    }

    scheduler_descriptor_exhaustion_proof();
    expect_scheduler_status(
        scheduler_yield(),
        SCHEDULER_STATUS_NO_RUNNABLE_PEER,
        "yield without a peer was not deterministic"
    );

    if (scheduler_get_stats(&stats) != SCHEDULER_STATUS_OK ||
        stats.ready_queue_entries != 0U || stats.ready_tasks != 0U ||
        stats.running_dynamic_tasks != 0U || stats.exited_tasks != 0U ||
        stats.mapped_stack_pages != 0U ||
        stats.task_stack_owned_frames != 0U ||
        !stats.bootstrap_running || stats.poisoned ||
        heap_get_stats(&heap_stats) != HEAP_STATUS_OK ||
        heap_stats.live_allocations != 0U ||
        heap_validate() != HEAP_STATUS_OK ||
        scheduler_validate() != SCHEDULER_STATUS_OK ||
        kernel_time_validate() != KERNEL_TIME_STATUS_OK ||
        pit_is_running() || !apic_timer_is_masked() || !pic_all_masked() ||
        !apic_local_timer_active()) {
        kernel_test_fail("final scheduler, heap, timer, or APIC state is invalid");
    }

    console_write("Zenith OS: bounded cooperative scheduler verified\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_scheduler_guard(void)
{
    struct scheduler_task_handle handle;
    struct scheduler_task_info info;
    struct virtual_memory_mapping mapping;

    if (active_scenario != KERNEL_TEST_SCHEDULER_GUARD) {
        kernel_test_fail("scheduler guard completion used outside its scenario");
    }

    expect_scheduler_status(
        scheduler_task_create(scheduler_noop_task, NULL, &handle),
        SCHEDULER_STATUS_OK,
        "scheduler guard task creation failed"
    );
    expect_scheduler_status(
        scheduler_task_query(handle, &info),
        SCHEDULER_STATUS_OK,
        "scheduler guard task query failed"
    );

    if (virtual_memory_query(info.lower_guard, &mapping) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        info.lower_guard + ZENITH_PAGE_SIZE != info.stack_start) {
        kernel_test_fail("selected scheduler guard is not absent");
    }

    scheduler_guard_expected_address = info.lower_guard;
    scheduler_test_trigger_guard((uintptr_t)info.lower_guard);
}

bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame)
{
    bool matches = false;

    if (frame == NULL) {
        return false;
    }

    switch (active_scenario) {
    case KERNEL_TEST_INVALID_OPCODE:
        matches = frame->vector == 6U &&
            frame->error_code == 0U &&
            frame->rip == (uintptr_t)(const void *)interrupt_invalid_opcode_site;
        break;
    case KERNEL_TEST_PAGE_FAULT:
        matches = frame->vector == 14U &&
            frame->error_code == 0U &&
            frame->cr2 == PAGE_FAULT_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)interrupt_page_fault_site;
        break;
    case KERNEL_TEST_UNEXPECTED:
        matches = frame->vector == UINT64_C(0x80) && frame->error_code == 0U;
        break;
    case KERNEL_TEST_WRITE_PROTECT:
        matches = frame->vector == 14U &&
            frame->error_code == (PAGE_FAULT_PRESENT | PAGE_FAULT_WRITE) &&
            frame->cr2 == (uintptr_t)(const void *)
                interrupt_write_protect_target &&
            frame->rip == (uintptr_t)(const void *)
                interrupt_write_protect_site;
        break;
    case KERNEL_TEST_NX:
        matches = frame->vector == 14U &&
            frame->error_code ==
                (PAGE_FAULT_PRESENT | PAGE_FAULT_INSTRUCTION) &&
            frame->cr2 == (uintptr_t)(const void *)
                interrupt_non_executable_target &&
            frame->rip == (uintptr_t)(const void *)
                interrupt_non_executable_target;
        break;
    case KERNEL_TEST_SCHEDULER_GUARD:
        matches = frame->vector == 14U &&
            frame->error_code == 0U &&
            frame->cr2 == scheduler_guard_expected_address &&
            frame->rip == (uintptr_t)(const void *)scheduler_guard_fault_site;
        break;
    default:
        return false;
    }

    if (!matches) {
        kernel_test_fail("fatal interrupt did not match its expectation");
    }

    kernel_test_pass();
}

const char *kernel_test_scenario_name(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NONE:
        return "none";
    case KERNEL_TEST_NORMAL:
        return "normal";
    case KERNEL_TEST_BREAKPOINT:
        return "breakpoint";
    case KERNEL_TEST_INVALID_OPCODE:
        return "invalid-opcode";
    case KERNEL_TEST_PAGE_FAULT:
        return "page-fault";
    case KERNEL_TEST_IST:
        return "ist";
    case KERNEL_TEST_PIT:
        return "pit";
    case KERNEL_TEST_UNEXPECTED:
        return "unexpected";
    case KERNEL_TEST_DOUBLE_FAULT:
        return "double-fault";
    case KERNEL_TEST_WRITE_PROTECT:
        return "write-protect";
    case KERNEL_TEST_NX:
        return "nx";
    case KERNEL_TEST_APIC:
        return "apic";
    case KERNEL_TEST_LAPIC_TIMER:
        return "lapic-timer";
    case KERNEL_TEST_HEAP:
        return "heap";
    case KERNEL_TEST_SCHEDULER:
        return "scheduler";
    case KERNEL_TEST_SCHEDULER_GUARD:
        return "scheduler-guard";
    case KERNEL_TEST_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

_Noreturn void kernel_test_fail(const char *reason)
{
    console_write("ZT FAIL ");
    console_write(kernel_test_scenario_name(active_scenario));
    console_write(": ");
    console_write(reason);
    console_putc('\n');
    cpu_out32(QEMU_EXIT_PORT, QEMU_FAILURE_VALUE);
    console_halt();
}

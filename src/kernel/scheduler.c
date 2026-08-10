/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/heap.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/scheduler.h>
#include <zenith/virtual_memory.h>

#include "scheduler_arch.h"
#include "scheduler_core.h"

#define SCHEDULER_STACK_PAGE_COUNT VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES
#define SCHEDULER_STACK_SIZE \
    ((uint64_t)SCHEDULER_STACK_PAGE_COUNT * ZENITH_PAGE_SIZE)
#define SCHEDULER_CONTEXT_STACK_MODULO ((uintptr_t)8U)

struct scheduler_runtime_task {
    struct scheduler_context context;
    scheduler_task_entry_t entry;
    void *opaque_context;
    uintptr_t frames[SCHEDULER_STACK_PAGE_COUNT];
    uint64_t lower_guard;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper_guard;
    uint64_t generation;
    bool populated;
    bool first_entry_started;
    bool resume_interrupts_enabled;
};

extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t bootstrap_stack_bottom[];
extern uint8_t bootstrap_stack_top[];

static struct scheduler_core_state active_core;
static struct scheduler_core_state candidate_core;
static struct scheduler_core_state self_test_core;
static struct scheduler_runtime_task runtime_tasks[SCHEDULER_TASK_LIMIT];
static struct scheduler_context bootstrap_context;
static uintptr_t transaction_frames[SCHEDULER_STACK_PAGE_COUNT];
static bool transaction_mapped[SCHEDULER_STACK_PAGE_COUNT];
static bool bootstrap_context_saved;
static bool bootstrap_resume_interrupts_enabled;
static bool scheduler_initialized;
static bool scheduler_poisoned;

_Static_assert(sizeof(struct scheduler_context) == 64U,
    "scheduler context ABI size changed");
_Static_assert(sizeof(struct scheduler_core_state) == 736U,
    "scheduler core BSS accounting changed");
_Static_assert(sizeof(struct scheduler_runtime_task) == 256U,
    "scheduler runtime-record BSS accounting changed");
_Static_assert(sizeof(scheduler_task_entry_t) == sizeof(uintptr_t),
    "scheduler entry pointers must fit in uintptr_t");
_Static_assert(offsetof(struct scheduler_context, rbx) == 0U,
    "scheduler context RBX offset changed");
_Static_assert(offsetof(struct scheduler_context, rbp) == 8U,
    "scheduler context RBP offset changed");
_Static_assert(offsetof(struct scheduler_context, r12) == 16U,
    "scheduler context R12 offset changed");
_Static_assert(offsetof(struct scheduler_context, r13) == 24U,
    "scheduler context R13 offset changed");
_Static_assert(offsetof(struct scheduler_context, r14) == 32U,
    "scheduler context R14 offset changed");
_Static_assert(offsetof(struct scheduler_context, r15) == 40U,
    "scheduler context R15 offset changed");
_Static_assert(offsetof(struct scheduler_context, rsp) == 48U,
    "scheduler context RSP offset changed");
_Static_assert(offsetof(struct scheduler_context, rip) == 56U,
    "scheduler context RIP offset changed");
_Static_assert(SCHEDULER_TASK_LIMIT == VIRTUAL_MEMORY_TASK_STACK_LIMIT,
    "scheduler descriptor and stack-slot limits disagree");
_Static_assert(SCHEDULER_STACK_SIZE == UINT64_C(65536),
    "dynamic task stack payload must remain 64 KiB");

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static bool forbidden_context(void)
{
    return interrupt_context_active() || console_panic_active();
}

static bool address_is_canonical(uint64_t address)
{
    const uint64_t upper = address >> 48U;

    return (address & (UINT64_C(1) << 47U)) == 0U
        ? upper == 0U
        : upper == UINT64_C(0xFFFF);
}

static bool kernel_text_address(uintptr_t address)
{
    const uintptr_t text_start = (uintptr_t)(const void *)__text_start;
    const uintptr_t text_end = (uintptr_t)(const void *)__text_end;

    return address >= text_start && address < text_end &&
        address_is_canonical((uint64_t)address);
}

static bool entry_is_valid(scheduler_task_entry_t entry)
{
    union {
        scheduler_task_entry_t function;
        uintptr_t address;
    } conversion;
    struct virtual_memory_mapping mapping;

    conversion.function = entry;

    return entry != NULL && kernel_text_address(conversion.address) &&
        virtual_memory_query((uint64_t)conversion.address, &mapping) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        mapping.permissions == VIRTUAL_MEMORY_EXECUTABLE;
}

static void runtime_task_clear(struct scheduler_runtime_task *task)
{
    bytes_zero(task, sizeof(*task));
}

static void runtime_task_copy(
    struct scheduler_runtime_task *destination,
    const struct scheduler_runtime_task *source
)
{
    destination->context.rbx = source->context.rbx;
    destination->context.rbp = source->context.rbp;
    destination->context.r12 = source->context.r12;
    destination->context.r13 = source->context.r13;
    destination->context.r14 = source->context.r14;
    destination->context.r15 = source->context.r15;
    destination->context.rsp = source->context.rsp;
    destination->context.rip = source->context.rip;
    destination->entry = source->entry;
    destination->opaque_context = source->opaque_context;

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        destination->frames[page] = source->frames[page];
    }

    destination->lower_guard = source->lower_guard;
    destination->stack_start = source->stack_start;
    destination->stack_end = source->stack_end;
    destination->upper_guard = source->upper_guard;
    destination->generation = source->generation;
    destination->populated = source->populated;
    destination->first_entry_started = source->first_entry_started;
    destination->resume_interrupts_enabled =
        source->resume_interrupts_enabled;
}

static bool runtime_task_is_clear(const struct scheduler_runtime_task *task)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)task;

    for (size_t index = 0U; index < sizeof(*task); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }

    return true;
}

static bool frame_is_task_stack_owned(uintptr_t frame)
{
    enum frame_owner owner = FRAME_OWNER_NONE;

    return frame != 0U &&
        frame_query_owner(frame, &owner) == FRAME_STATUS_OK &&
        owner == FRAME_OWNER_TASK_STACK;
}

static bool frame_is_unique(
    uintptr_t frame,
    size_t task_limit,
    size_t page_limit
)
{
    for (size_t task_index = 0U;
         task_index < task_limit;
         ++task_index) {
        const struct scheduler_runtime_task *task =
            &runtime_tasks[task_index];
        size_t pages = task_index + 1U == task_limit
            ? page_limit
            : SCHEDULER_STACK_PAGE_COUNT;

        if (!task->populated) {
            continue;
        }

        for (size_t page = 0U; page < pages; ++page) {
            if (task->frames[page] == frame) {
                return false;
            }
        }
    }

    return true;
}

static bool mapping_matches(
    size_t slot_index,
    size_t page_index,
    uintptr_t frame
)
{
    struct virtual_memory_mapping mapping;

    return virtual_memory_query_task_stack_page(
            slot_index,
            page_index,
            &mapping
        ) == VIRTUAL_MEMORY_STATUS_OK &&
        mapping.physical_address == (uint64_t)frame &&
        mapping.permissions == VIRTUAL_MEMORY_WRITABLE;
}

static bool guards_absent(size_t slot_index)
{
    struct virtual_memory_mapping mapping;
    uint64_t lower;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper;

    return virtual_memory_task_stack_bounds(
            slot_index,
            &lower,
            &stack_start,
            &stack_end,
            &upper
        ) == VIRTUAL_MEMORY_STATUS_OK &&
        virtual_memory_query(lower, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED &&
        virtual_memory_query(upper, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED;
}

static bool saved_context_is_valid(
    const struct scheduler_context *context,
    uintptr_t stack_start,
    uintptr_t stack_end,
    bool first_entry_allowed
)
{
    uintptr_t return_address;
    const uintptr_t first_entry =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;
    const uintptr_t resume =
        (uintptr_t)(const void *)scheduler_context_switch_resume;

    if (context == NULL || stack_start >= stack_end ||
        !address_is_canonical((uint64_t)context->rsp) ||
        context->rsp < stack_start || context->rsp > stack_end - 8U ||
        (context->rsp & (uintptr_t)0xFU) !=
            SCHEDULER_CONTEXT_STACK_MODULO ||
        (context->rip != resume &&
            (!first_entry_allowed || context->rip != first_entry))) {
        return false;
    }

    return_address = *(const volatile uintptr_t *)(uintptr_t)context->rsp;

    if (context->rip == first_entry) {
        return return_address ==
            (uintptr_t)(const void *)scheduler_task_return_trampoline_address;
    }

    return kernel_text_address(return_address);
}

static bool running_stack_is_valid(uintptr_t stack_start, uintptr_t stack_end)
{
    const uintptr_t stack_pointer = cpu_read_stack_pointer();

    return stack_start < stack_end &&
        address_is_canonical((uint64_t)stack_pointer) &&
        stack_pointer >= stack_start && stack_pointer < stack_end &&
        (stack_pointer & (uintptr_t)0xFU) ==
            SCHEDULER_CONTEXT_STACK_MODULO;
}

static enum virtual_memory_status map_page_preserving_interrupts(
    size_t slot_index,
    size_t page_index,
    uintptr_t frame
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum virtual_memory_status status;

    cpu_interrupt_disable();
    status = virtual_memory_map_task_stack_page(
        slot_index,
        page_index,
        frame
    );

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    return status;
}

static enum virtual_memory_status unmap_page_preserving_interrupts(
    size_t slot_index,
    size_t page_index,
    uintptr_t frame
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum virtual_memory_status status;

    cpu_interrupt_disable();
    status = virtual_memory_unmap_task_stack_page(
        slot_index,
        page_index,
        frame
    );

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    return status;
}

static enum scheduler_status status_from_core(enum scheduler_core_status status)
{
    switch (status) {
    case SCHEDULER_CORE_OK:
        return SCHEDULER_STATUS_OK;
    case SCHEDULER_CORE_NULL_ARGUMENT:
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    case SCHEDULER_CORE_ALREADY_INITIALIZED:
        return SCHEDULER_STATUS_ALREADY_INITIALIZED;
    case SCHEDULER_CORE_NOT_INITIALIZED:
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    case SCHEDULER_CORE_POISONED:
        return SCHEDULER_STATUS_POISONED;
    case SCHEDULER_CORE_INVALID_HANDLE:
        return SCHEDULER_STATUS_INVALID_HANDLE;
    case SCHEDULER_CORE_STALE_HANDLE:
        return SCHEDULER_STATUS_STALE_HANDLE;
    case SCHEDULER_CORE_GENERATION_EXHAUSTED:
        return SCHEDULER_STATUS_GENERATION_EXHAUSTED;
    case SCHEDULER_CORE_DESCRIPTOR_LIMIT:
        return SCHEDULER_STATUS_DESCRIPTOR_LIMIT;
    case SCHEDULER_CORE_QUEUE_LIMIT:
        return SCHEDULER_STATUS_QUEUE_LIMIT;
    case SCHEDULER_CORE_NO_RUNNABLE_PEER:
        return SCHEDULER_STATUS_NO_RUNNABLE_PEER;
    case SCHEDULER_CORE_INVALID_STATE:
        return SCHEDULER_STATUS_INVALID_STATE;
    case SCHEDULER_CORE_RUNNING_TASK:
        return SCHEDULER_STATUS_RUNNING_TASK;
    case SCHEDULER_CORE_RUNNABLE_TASK:
        return SCHEDULER_STATUS_RUNNABLE_TASK;
    case SCHEDULER_CORE_DOUBLE_REAP:
        return SCHEDULER_STATUS_DOUBLE_REAP;
    case SCHEDULER_CORE_BOOTSTRAP_TASK:
        return SCHEDULER_STATUS_BOOTSTRAP_TASK;
    case SCHEDULER_CORE_STATS_INVALID:
        return SCHEDULER_STATUS_STATS_INVALID;
    case SCHEDULER_CORE_CORRUPTED:
    default:
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }
}

static void poison_scheduler(void)
{
    scheduler_poisoned = true;
    active_core.poisoned = true;
}

static bool runtime_task_is_valid(
    size_t task_index,
    const struct scheduler_core_state *core
)
{
    const struct scheduler_runtime_task *task = &runtime_tasks[task_index];
    uint64_t lower;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper;

    if (!task->populated || task->generation == 0U ||
        task->generation != core->descriptors[task_index].generation ||
        !entry_is_valid(task->entry) ||
        virtual_memory_task_stack_bounds(
            task_index,
            &lower,
            &stack_start,
            &stack_end,
            &upper
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        task->lower_guard != lower || task->stack_start != stack_start ||
        task->stack_end != stack_end || task->upper_guard != upper ||
        stack_end - stack_start != SCHEDULER_STACK_SIZE ||
        (core->descriptors[task_index].state == SCHEDULER_TASK_RUNNING
            ? (!core->current.bootstrap &&
                core->current.index == task_index &&
                core->current.generation == task->generation &&
                running_stack_is_valid(
                    (uintptr_t)stack_start,
                    (uintptr_t)stack_end
                ))
            : saved_context_is_valid(
                &task->context,
                (uintptr_t)stack_start,
                (uintptr_t)stack_end,
                true
            )) == false ||
        !guards_absent(task_index)) {
        return false;
    }

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        if (!frame_is_task_stack_owned(task->frames[page]) ||
            !frame_is_unique(task->frames[page], task_index + 1U, page) ||
            !mapping_matches(task_index, page, task->frames[page])) {
            return false;
        }
    }

    return true;
}

static enum scheduler_status validate_state(
    const struct scheduler_core_state *core
)
{
    const struct frame_allocator_stats frame_stats =
        frame_allocator_get_stats();
    struct virtual_memory_runtime_stats vm_stats;
    size_t live_task_count = 0U;
    enum scheduler_core_status core_status;

    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }

    if (scheduler_poisoned || core->poisoned) {
        return SCHEDULER_STATUS_POISONED;
    }

    core_status = scheduler_core_validate(core);

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    if (frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_runtime_get_stats(&vm_stats) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        heap_validate() != HEAP_STATUS_OK ||
        vm_stats.task_stack_payload_pages !=
            SCHEDULER_TASK_LIMIT * SCHEDULER_STACK_PAGE_COUNT ||
        vm_stats.table_pages_used > vm_stats.table_pages_capacity) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    if ((core->bootstrap_state == SCHEDULER_TASK_RUNNING &&
            !running_stack_is_valid(
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top
            )) ||
        (core->bootstrap_state == SCHEDULER_TASK_READY &&
            (!bootstrap_context_saved ||
                !saved_context_is_valid(
                    &bootstrap_context,
                    (uintptr_t)(void *)bootstrap_stack_bottom,
                    (uintptr_t)(void *)bootstrap_stack_top,
                    false
                )))) {
        return SCHEDULER_STATUS_CONTEXT_FAILURE;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const enum scheduler_task_state state =
            core->descriptors[index].state;

        switch (state) {
        case SCHEDULER_TASK_READY:
        case SCHEDULER_TASK_RUNNING:
        case SCHEDULER_TASK_EXITED:
            if (!runtime_task_is_valid(index, core)) {
                return SCHEDULER_STATUS_VALIDATION_FAILURE;
            }

            ++live_task_count;
            break;
        case SCHEDULER_TASK_UNUSED:
        case SCHEDULER_TASK_RETIRED:
            if (!runtime_task_is_clear(&runtime_tasks[index])) {
                return SCHEDULER_STATUS_VALIDATION_FAILURE;
            }

            for (size_t page = 0U;
                 page < SCHEDULER_STACK_PAGE_COUNT;
                 ++page) {
                struct virtual_memory_mapping mapping;

                if (virtual_memory_query_task_stack_page(
                        index,
                        page,
                        &mapping
                    ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
                    return SCHEDULER_STATUS_MAPPING_FAILURE;
                }
            }

            if (!guards_absent(index)) {
                return SCHEDULER_STATUS_MAPPING_FAILURE;
            }
            break;
        case SCHEDULER_TASK_CONSTRUCTING:
        case SCHEDULER_TASK_REAPING:
        case SCHEDULER_TASK_POISONED:
        default:
            return SCHEDULER_STATUS_INVALID_STATE;
        }
    }

    if (live_task_count > SCHEDULER_TASK_LIMIT ||
        live_task_count > SIZE_MAX / SCHEDULER_STACK_PAGE_COUNT ||
        vm_stats.task_stack_mapped_pages !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT ||
        frame_stats.task_stack_allocated_frames !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT ||
        frame_stats.allocated_frames !=
            frame_stats.generic_allocated_frames +
                frame_stats.heap_allocated_frames +
                frame_stats.task_stack_allocated_frames) {
        return SCHEDULER_STATUS_STATS_INVALID;
    }

    return SCHEDULER_STATUS_OK;
}

static size_t active_live_task_count(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        switch (active_core.descriptors[index].state) {
        case SCHEDULER_TASK_READY:
        case SCHEDULER_TASK_RUNNING:
        case SCHEDULER_TASK_EXITED:
            ++count;
            break;
        default:
            break;
        }
    }

    return count;
}

static bool mapped_stack_is_zero(const struct scheduler_runtime_task *task)
{
    const volatile uint8_t *bytes =
        (const volatile uint8_t *)(uintptr_t)task->stack_start;

    for (size_t index = 0U; index < (size_t)SCHEDULER_STACK_SIZE; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }

    return true;
}

static enum scheduler_status validate_construction(
    struct scheduler_task_handle handle
)
{
    const struct frame_allocator_stats frame_stats =
        frame_allocator_get_stats();
    struct virtual_memory_runtime_stats vm_stats;
    size_t previous_live_tasks = active_live_task_count();

    if (handle.index >= SCHEDULER_TASK_LIMIT ||
        candidate_core.descriptors[handle.index].state !=
            SCHEDULER_TASK_CONSTRUCTING ||
        !runtime_task_is_valid(handle.index, &candidate_core) ||
        frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_runtime_get_stats(&vm_stats) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        previous_live_tasks >= SCHEDULER_TASK_LIMIT ||
        vm_stats.task_stack_mapped_pages !=
            (previous_live_tasks + 1U) * SCHEDULER_STACK_PAGE_COUNT ||
        frame_stats.task_stack_allocated_frames !=
            (previous_live_tasks + 1U) * SCHEDULER_STACK_PAGE_COUNT) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    return SCHEDULER_STATUS_OK;
}

static struct scheduler_context *context_for_identity(
    const struct scheduler_core_identity *identity
)
{
    return identity->bootstrap
        ? &bootstrap_context
        : &runtime_tasks[identity->index].context;
}

static bool target_context_is_valid(
    const struct scheduler_core_identity *identity
)
{
    if (identity->bootstrap) {
        return bootstrap_context_saved &&
            saved_context_is_valid(
                &bootstrap_context,
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top,
                false
            );
    }

    if (identity->index >= SCHEDULER_TASK_LIMIT ||
        runtime_tasks[identity->index].generation != identity->generation) {
        return false;
    }

    return saved_context_is_valid(
        &runtime_tasks[identity->index].context,
        (uintptr_t)runtime_tasks[identity->index].stack_start,
        (uintptr_t)runtime_tasks[identity->index].stack_end,
        true
    );
}

static void clear_transaction(size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        transaction_frames[index] = 0U;
        transaction_mapped[index] = false;
    }
}

static enum scheduler_status rollback_creation(
    struct scheduler_task_handle handle,
    size_t allocated_count
)
{
    bool rollback_failed = false;
    struct scheduler_task_handle rollback_handle;
    enum scheduler_core_status core_status;

    for (size_t count = allocated_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (transaction_mapped[page]) {
            enum virtual_memory_status vm_status =
                unmap_page_preserving_interrupts(
                    handle.index,
                    page,
                    transaction_frames[page]
                );

            if (vm_status == VIRTUAL_MEMORY_STATUS_OK) {
                transaction_mapped[page] = false;
            } else {
                rollback_failed = true;
            }
        }

        if (!transaction_mapped[page] && transaction_frames[page] != 0U) {
            if (frame_release_owned(
                    transaction_frames[page],
                    FRAME_OWNER_TASK_STACK
                ) != FRAME_STATUS_OK) {
                rollback_failed = true;
            } else {
                transaction_frames[page] = 0U;
            }
        }
    }

    runtime_task_clear(&runtime_tasks[handle.index]);

    if (rollback_failed) {
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
    }

    clear_transaction(allocated_count);
    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_begin_create(
        &candidate_core,
        &rollback_handle
    );

    if (core_status != SCHEDULER_CORE_OK ||
        rollback_handle.index != handle.index ||
        rollback_handle.generation != handle.generation ||
        scheduler_core_abort_create(&candidate_core, rollback_handle) !=
            SCHEDULER_CORE_OK ||
        validate_state(&candidate_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status construct_task_stack(
    struct scheduler_task_handle handle,
    scheduler_task_entry_t entry,
    void *opaque_context
)
{
    struct scheduler_runtime_task *task = &runtime_tasks[handle.index];
    size_t allocated_count = 0U;

    runtime_task_clear(task);
    clear_transaction(SCHEDULER_STACK_PAGE_COUNT);

    if (virtual_memory_task_stack_bounds(
            handle.index,
            &task->lower_guard,
            &task->stack_start,
            &task->stack_end,
            &task->upper_guard
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        task->stack_end - task->stack_start != SCHEDULER_STACK_SIZE) {
        enum scheduler_status rollback_status = rollback_creation(
            handle,
            0U
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_ARITHMETIC_OVERFLOW
            : rollback_status;
    }

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        volatile uint8_t *page_bytes;
        enum frame_status frame_status = frame_allocate_owned(
            FRAME_OWNER_TASK_STACK,
            &transaction_frames[page]
        );
        enum virtual_memory_status vm_status;

        if (frame_status != FRAME_STATUS_OK) {
            enum scheduler_status rollback_status = rollback_creation(
                handle,
                allocated_count
            );

            if (rollback_status != SCHEDULER_STATUS_OK) {
                return rollback_status;
            }

            return frame_status == FRAME_STATUS_OUT_OF_MEMORY
                ? SCHEDULER_STATUS_OUT_OF_MEMORY
                : frame_status == FRAME_STATUS_TEST_FAILURE
                    ? SCHEDULER_STATUS_TEST_FAILURE
                    : SCHEDULER_STATUS_FRAME_FAILURE;
        }

        ++allocated_count;
        vm_status = map_page_preserving_interrupts(
            handle.index,
            page,
            transaction_frames[page]
        );

        if (vm_status != VIRTUAL_MEMORY_STATUS_OK) {
            if (vm_status == VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE) {
                transaction_mapped[page] = true;
            }

            {
                enum scheduler_status rollback_status = rollback_creation(
                    handle,
                    allocated_count
                );

                if (rollback_status != SCHEDULER_STATUS_OK) {
                    return rollback_status;
                }
            }

            return vm_status == VIRTUAL_MEMORY_STATUS_TEST_FAILURE
                ? SCHEDULER_STATUS_TEST_FAILURE
                : SCHEDULER_STATUS_MAPPING_FAILURE;
        }

        transaction_mapped[page] = true;
        page_bytes = (volatile uint8_t *)(uintptr_t)(
            task->stack_start + (uint64_t)page * ZENITH_PAGE_SIZE
        );

        for (size_t byte = 0U; byte < (size_t)ZENITH_PAGE_SIZE; ++byte) {
            page_bytes[byte] = 0U;
        }
    }

    task->entry = entry;
    task->opaque_context = opaque_context;
    task->generation = handle.generation;
    task->populated = true;

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        task->frames[page] = transaction_frames[page];
    }

    if (!mapped_stack_is_zero(task)) {
        enum scheduler_status rollback_status = rollback_creation(
            handle,
            allocated_count
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : rollback_status;
    }

    task->context.rsp = (uintptr_t)(task->stack_end - 8U);
    task->context.rip =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;
    *(volatile uintptr_t *)(uintptr_t)task->context.rsp =
        (uintptr_t)(const void *)scheduler_task_return_trampoline_address;

    if (!saved_context_is_valid(
            &task->context,
            (uintptr_t)task->stack_start,
            (uintptr_t)task->stack_end,
            true
        ) || validate_construction(handle) != SCHEDULER_STATUS_OK) {
        enum scheduler_status rollback_status = rollback_creation(
            handle,
            allocated_count
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : rollback_status;
    }

    return SCHEDULER_STATUS_OK;
}

bool scheduler_self_test(void)
{
    _Alignas(16) uintptr_t fake_stack[4] = {0U, 0U, 0U, 0U};
    struct scheduler_context fake_context;
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    struct scheduler_task_handle handle;
    enum scheduler_task_state state;
    uint64_t previous_slot_end = 0U;
    uint64_t lower;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper;

    bytes_zero(&fake_context, sizeof(fake_context));
    fake_context.rsp = (uintptr_t)(void *)&fake_stack[1];
    fake_context.rip =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;
    fake_stack[1] =
        (uintptr_t)(const void *)scheduler_task_return_trampoline_address;

    if (!saved_context_is_valid(
            &fake_context,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[4],
            true
        )) {
        return false;
    }

    fake_context.rsp = (uintptr_t)(void *)&fake_stack[0];

    if (saved_context_is_valid(
            &fake_context,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[4],
            true
        )) {
        return false;
    }

    fake_context.rsp = (uintptr_t)(void *)&fake_stack[4];

    if (saved_context_is_valid(
            &fake_context,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[4],
            true
        )) {
        return false;
    }

    for (size_t slot = 0U; slot < SCHEDULER_TASK_LIMIT; ++slot) {
        if (virtual_memory_task_stack_bounds(
                slot,
                &lower,
                &stack_start,
                &stack_end,
                &upper
            ) != VIRTUAL_MEMORY_STATUS_OK ||
            stack_start != lower + ZENITH_PAGE_SIZE ||
            stack_end != upper ||
            stack_end - stack_start != SCHEDULER_STACK_SIZE ||
            (slot != 0U && lower != previous_slot_end)) {
            return false;
        }

        previous_slot_end = upper + ZENITH_PAGE_SIZE;
    }

    if (previous_slot_end != VIRTUAL_MEMORY_TASK_STACK_WINDOW_END) {
        return false;
    }

    scheduler_core_clear(&self_test_core);

    if (scheduler_initialized ||
        scheduler_core_initialize(&self_test_core) != SCHEDULER_CORE_OK ||
        scheduler_core_initialize(&self_test_core) !=
            SCHEDULER_CORE_ALREADY_INITIALIZED ||
        scheduler_core_yield(&self_test_core, &previous, &next) !=
            SCHEDULER_CORE_NO_RUNNABLE_PEER ||
        scheduler_core_begin_create(&self_test_core, &handle) !=
            SCHEDULER_CORE_OK ||
        handle.index != 0U || handle.generation != 1U ||
        scheduler_core_publish_create(&self_test_core, handle) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_yield(&self_test_core, &previous, &next) !=
            SCHEDULER_CORE_OK ||
        !previous.bootstrap || next.bootstrap || next.index != 0U ||
        scheduler_core_exit_current(&self_test_core, &previous, &next) !=
            SCHEDULER_CORE_OK ||
        previous.bootstrap || !next.bootstrap ||
        scheduler_core_begin_reap(&self_test_core, handle) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_finish_reap(&self_test_core, handle) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_resolve_handle(&self_test_core, handle, &state) !=
            SCHEDULER_CORE_STALE_HANDLE ||
        virtual_memory_task_stack_bounds(
            0U,
            &lower,
            &stack_start,
            &stack_end,
            &upper
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        lower != VIRTUAL_MEMORY_TASK_STACK_BASE ||
        stack_start != lower + ZENITH_PAGE_SIZE ||
        stack_end != stack_start + SCHEDULER_STACK_SIZE ||
        upper != stack_end ||
        virtual_memory_task_stack_bounds(
            SCHEDULER_TASK_LIMIT - 1U,
            &lower,
            &stack_start,
            &stack_end,
            &upper
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        upper + ZENITH_PAGE_SIZE !=
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END ||
        virtual_memory_task_stack_bounds(
            SCHEDULER_TASK_LIMIT,
            &lower,
            &stack_start,
            &stack_end,
            &upper
        ) != VIRTUAL_MEMORY_STATUS_BAD_STACK_SLOT) {
        return false;
    }

    scheduler_core_clear(&self_test_core);

    if (scheduler_core_initialize(&self_test_core) != SCHEDULER_CORE_OK) {
        return false;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        self_test_core.descriptors[index].state = SCHEDULER_TASK_RETIRED;
        self_test_core.descriptors[index].generation = UINT64_MAX;
    }

    return scheduler_core_validate(&self_test_core) == SCHEDULER_CORE_OK &&
        scheduler_core_begin_create(&self_test_core, &handle) ==
            SCHEDULER_CORE_GENERATION_EXHAUSTED;
}

enum scheduler_status scheduler_initialize(void)
{
    struct virtual_memory_runtime_stats vm_stats;
    const struct frame_allocator_stats frame_stats =
        frame_allocator_get_stats();
    const uintptr_t stack_pointer = cpu_read_stack_pointer();
    enum scheduler_core_status core_status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (scheduler_initialized) {
        return SCHEDULER_STATUS_ALREADY_INITIALIZED;
    }

    if ((uintptr_t)(void *)bootstrap_stack_bottom >=
            (uintptr_t)(void *)bootstrap_stack_top ||
        stack_pointer < (uintptr_t)(void *)bootstrap_stack_bottom ||
        stack_pointer >= (uintptr_t)(void *)bootstrap_stack_top ||
        frame_allocator_validate() != FRAME_STATUS_OK ||
        virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_runtime_get_stats(&vm_stats) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        heap_validate() != HEAP_STATUS_OK ||
        vm_stats.task_stack_payload_pages !=
            SCHEDULER_TASK_LIMIT * SCHEDULER_STACK_PAGE_COUNT ||
        vm_stats.task_stack_mapped_pages != 0U ||
        frame_stats.task_stack_allocated_frames != 0U) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    scheduler_core_clear(&active_core);
    scheduler_core_clear(&candidate_core);
    bytes_zero(&bootstrap_context, sizeof(bootstrap_context));

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        runtime_task_clear(&runtime_tasks[index]);
    }

    clear_transaction(SCHEDULER_STACK_PAGE_COUNT);
    bootstrap_context_saved = false;
    bootstrap_resume_interrupts_enabled = false;
    scheduler_poisoned = false;
    core_status = scheduler_core_initialize(&active_core);

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    scheduler_initialized = true;

    if (validate_state(&active_core) != SCHEDULER_STATUS_OK) {
        scheduler_initialized = false;
        scheduler_core_clear(&active_core);
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_task_create(
    scheduler_task_entry_t entry,
    void *opaque_context,
    struct scheduler_task_handle *handle
)
{
    struct scheduler_task_handle candidate_handle;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (handle == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    handle->index = SCHEDULER_INVALID_INDEX;
    handle->generation = 0U;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }

    if (scheduler_poisoned) {
        return SCHEDULER_STATUS_POISONED;
    }

    if (!entry_is_valid(entry)) {
        return SCHEDULER_STATUS_INVALID_ENTRY;
    }

    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_begin_create(
        &candidate_core,
        &candidate_handle
    );

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    status = construct_task_stack(
        candidate_handle,
        entry,
        opaque_context
    );

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    core_status = scheduler_core_publish_create(
        &candidate_core,
        candidate_handle
    );

    if (core_status != SCHEDULER_CORE_OK ||
        validate_state(&candidate_core) != SCHEDULER_STATUS_OK) {
        enum scheduler_status rollback_status = rollback_creation(
            candidate_handle,
            SCHEDULER_STACK_PAGE_COUNT
        );

        if (rollback_status != SCHEDULER_STATUS_OK) {
            return rollback_status;
        }

        return core_status == SCHEDULER_CORE_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : status_from_core(core_status);
    }

    scheduler_core_copy(&active_core, &candidate_core);
    clear_transaction(SCHEDULER_STACK_PAGE_COUNT);
    *handle = candidate_handle;
    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_yield(void)
{
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }

    if (scheduler_poisoned) {
        return SCHEDULER_STATUS_POISONED;
    }

    cpu_interrupt_disable();
    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        return status;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_yield(
        &candidate_core,
        &previous,
        &next
    );

    if (core_status != SCHEDULER_CORE_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        return status_from_core(core_status);
    }

    if (!target_context_is_valid(&next)) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }

        return SCHEDULER_STATUS_CONTEXT_FAILURE;
    }

    if (previous.bootstrap) {
        bootstrap_context_saved = true;
        bootstrap_resume_interrupts_enabled = interrupts_were_enabled;
    } else {
        runtime_tasks[previous.index].resume_interrupts_enabled =
            interrupts_were_enabled;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    scheduler_context_switch(
        context_for_identity(&previous),
        context_for_identity(&next)
    );

    if ((previous.bootstrap &&
            bootstrap_resume_interrupts_enabled != interrupts_were_enabled) ||
        (!previous.bootstrap &&
            runtime_tasks[previous.index].resume_interrupts_enabled !=
                interrupts_were_enabled)) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    } else {
        status = validate_state(&active_core);
    }

    if (status != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    }

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    return status;
}

_Noreturn void scheduler_task_first_entry_c(void)
{
    struct scheduler_runtime_task *task;
    const struct scheduler_core_identity current = active_core.current;

    if (!scheduler_initialized || scheduler_poisoned ||
        cpu_interrupts_enabled() || current.bootstrap ||
        current.index >= SCHEDULER_TASK_LIMIT ||
        active_core.descriptors[current.index].state !=
            SCHEDULER_TASK_RUNNING ||
        runtime_tasks[current.index].generation != current.generation ||
        runtime_tasks[current.index].first_entry_started ||
        validate_state(&active_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        console_panic("scheduler first-entry context is invalid");
    }

    task = &runtime_tasks[current.index];
    task->first_entry_started = true;
    cpu_interrupt_enable();
    task->entry(task->opaque_context);
    scheduler_task_exit();
}

_Noreturn void scheduler_task_exit(void)
{
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    enum scheduler_core_status core_status;

    if (forbidden_context() || !scheduler_initialized || scheduler_poisoned) {
        poison_scheduler();
        console_panic("scheduler task exit used in a forbidden state");
    }

    cpu_interrupt_disable();

    if (validate_state(&active_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        console_panic("scheduler task exit validation failed");
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_exit_current(
        &candidate_core,
        &previous,
        &next
    );

    if (core_status != SCHEDULER_CORE_OK || previous.bootstrap ||
        !target_context_is_valid(&next)) {
        poison_scheduler();
        console_panic("scheduler task exit could not select a safe peer");
    }

    scheduler_core_copy(&active_core, &candidate_core);
    scheduler_context_switch(
        context_for_identity(&previous),
        context_for_identity(&next)
    );
    poison_scheduler();
    console_panic("exited scheduler task resumed");
}

static bool exact_stack_mapping_present(
    size_t slot_index,
    size_t page_index,
    uintptr_t frame
)
{
    return frame_is_task_stack_owned(frame) &&
        mapping_matches(slot_index, page_index, frame);
}

static enum scheduler_status restore_reap_mappings(
    size_t slot_index,
    const struct scheduler_runtime_task *task,
    const bool *unmapped
)
{
    bool restore_failed = false;

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        enum virtual_memory_status vm_status;

        if (!unmapped[page]) {
            continue;
        }

        vm_status = map_page_preserving_interrupts(
            slot_index,
            page,
            task->frames[page]
        );

        if (vm_status != VIRTUAL_MEMORY_STATUS_OK &&
            !exact_stack_mapping_present(
                slot_index,
                page,
                task->frames[page]
            )) {
            restore_failed = true;
        }
    }

    return restore_failed
        ? SCHEDULER_STATUS_ROLLBACK_FAILURE
        : SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_task_reap(
    struct scheduler_task_handle handle
)
{
    bool unmapped[SCHEDULER_STACK_PAGE_COUNT] = {false};
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct scheduler_runtime_task saved_task;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }

    if (scheduler_poisoned) {
        return SCHEDULER_STATUS_POISONED;
    }

    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_begin_reap(&candidate_core, handle);

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    runtime_task_copy(&saved_task, &runtime_tasks[handle.index]);

    if (!saved_task.populated ||
        saved_task.generation != handle.generation ||
        !runtime_task_is_valid(handle.index, &active_core)) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    for (size_t count = SCHEDULER_STACK_PAGE_COUNT; count > 0U; --count) {
        const size_t page = count - 1U;
        enum virtual_memory_status vm_status =
            unmap_page_preserving_interrupts(
                handle.index,
                page,
                saved_task.frames[page]
            );

        if (vm_status == VIRTUAL_MEMORY_STATUS_OK) {
            unmapped[page] = true;
            continue;
        }

        if (vm_status == VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE &&
            !exact_stack_mapping_present(
                handle.index,
                page,
                saved_task.frames[page]
            )) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }

        status = restore_reap_mappings(handle.index, &saved_task, unmapped);

        if (status != SCHEDULER_STATUS_OK ||
            scheduler_core_abort_reap(&candidate_core, handle) !=
                SCHEDULER_CORE_OK ||
            validate_state(&active_core) != SCHEDULER_STATUS_OK) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }

        if (interrupts_were_enabled != cpu_interrupts_enabled()) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }

        return SCHEDULER_STATUS_MAPPING_FAILURE;
    }

    for (size_t count = SCHEDULER_STACK_PAGE_COUNT; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release_owned(
                saved_task.frames[page],
                FRAME_OWNER_TASK_STACK
            ) != FRAME_STATUS_OK) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }
    }

    runtime_task_clear(&runtime_tasks[handle.index]);
    core_status = scheduler_core_finish_reap(&candidate_core, handle);

    if (core_status != SCHEDULER_CORE_OK ||
        validate_state(&candidate_core) != SCHEDULER_STATUS_OK ||
        interrupts_were_enabled != cpu_interrupts_enabled()) {
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_current_task(
    struct scheduler_task_identity *identity
)
{
    enum scheduler_status status;

    if (identity == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    identity->bootstrap = false;
    identity->handle.index = SCHEDULER_INVALID_INDEX;
    identity->handle.generation = 0U;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    identity->bootstrap = active_core.current.bootstrap;

    if (!identity->bootstrap) {
        identity->handle.index = active_core.current.index;
        identity->handle.generation = active_core.current.generation;
    }

    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_task_query(
    struct scheduler_task_handle handle,
    struct scheduler_task_info *info
)
{
    enum scheduler_task_state state;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (info == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    bytes_zero(info, sizeof(*info));

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    core_status = scheduler_core_resolve_handle(
        &active_core,
        handle,
        &state
    );

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    if (state == SCHEDULER_TASK_UNUSED || state == SCHEDULER_TASK_RETIRED) {
        return SCHEDULER_STATUS_INVALID_STATE;
    }

    info->state = state;
    info->lower_guard = runtime_tasks[handle.index].lower_guard;
    info->stack_start = runtime_tasks[handle.index].stack_start;
    info->stack_end = runtime_tasks[handle.index].stack_end;
    info->upper_guard = runtime_tasks[handle.index].upper_guard;
    info->saved_stack_pointer =
        runtime_tasks[handle.index].context.rsp;
    info->queued = active_core.descriptors[handle.index].queued;
    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_validate(void)
{
    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    return validate_state(&active_core);
}

enum scheduler_status scheduler_get_stats(struct scheduler_stats *stats)
{
    const struct frame_allocator_stats frame_stats =
        frame_allocator_get_stats();
    struct virtual_memory_runtime_stats vm_stats;
    enum scheduler_status status;

    if (stats == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    bytes_zero(stats, sizeof(*stats));

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    if (virtual_memory_runtime_get_stats(&vm_stats) !=
            VIRTUAL_MEMORY_STATUS_OK) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    stats->dynamic_task_limit = SCHEDULER_TASK_LIMIT;
    stats->ready_queue_entries = active_core.queue_count;
    stats->mapped_stack_pages = vm_stats.task_stack_mapped_pages;
    stats->task_stack_owned_frames =
        frame_stats.task_stack_allocated_frames;
    stats->successful_creations = active_core.successful_creations;
    stats->failed_creations = active_core.failed_creations;
    stats->context_switches = active_core.context_switches;
    stats->completed_tasks = active_core.completed_tasks;
    stats->reaped_tasks = active_core.reaped_tasks;
    stats->bootstrap_running =
        active_core.bootstrap_state == SCHEDULER_TASK_RUNNING;
    stats->poisoned = scheduler_poisoned;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        switch (active_core.descriptors[index].state) {
        case SCHEDULER_TASK_UNUSED:
            ++stats->unused_tasks;
            break;
        case SCHEDULER_TASK_CONSTRUCTING:
            ++stats->constructing_tasks;
            break;
        case SCHEDULER_TASK_READY:
            ++stats->ready_tasks;
            break;
        case SCHEDULER_TASK_RUNNING:
            ++stats->running_dynamic_tasks;
            break;
        case SCHEDULER_TASK_EXITED:
            ++stats->exited_tasks;
            break;
        case SCHEDULER_TASK_REAPING:
            ++stats->reaping_tasks;
            break;
        case SCHEDULER_TASK_RETIRED:
            ++stats->retired_tasks;
            break;
        case SCHEDULER_TASK_POISONED:
        default:
            ++stats->poisoned_tasks;
            break;
        }
    }

    if (stats->unused_tasks + stats->constructing_tasks +
            stats->ready_tasks + stats->running_dynamic_tasks +
            stats->exited_tasks + stats->reaping_tasks +
            stats->retired_tasks + stats->poisoned_tasks !=
            SCHEDULER_TASK_LIMIT ||
        stats->ready_tasks +
            (active_core.bootstrap_state == SCHEDULER_TASK_READY ? 1U : 0U) !=
                stats->ready_queue_entries) {
        return SCHEDULER_STATUS_STATS_INVALID;
    }

    return SCHEDULER_STATUS_OK;
}

const char *scheduler_status_string(enum scheduler_status status)
{
    switch (status) {
    case SCHEDULER_STATUS_OK:
        return "ok";
    case SCHEDULER_STATUS_NULL_ARGUMENT:
        return "null scheduler argument";
    case SCHEDULER_STATUS_ALREADY_INITIALIZED:
        return "scheduler was initialized twice";
    case SCHEDULER_STATUS_NOT_INITIALIZED:
        return "scheduler is not initialized";
    case SCHEDULER_STATUS_FORBIDDEN_CONTEXT:
        return "scheduler use is forbidden in interrupt or panic context";
    case SCHEDULER_STATUS_POISONED:
        return "scheduler is permanently poisoned";
    case SCHEDULER_STATUS_INVALID_ENTRY:
        return "task entry is not executable kernel text";
    case SCHEDULER_STATUS_INVALID_HANDLE:
        return "task handle index or generation is invalid";
    case SCHEDULER_STATUS_STALE_HANDLE:
        return "task handle generation is stale";
    case SCHEDULER_STATUS_GENERATION_EXHAUSTED:
        return "all reusable task generations are exhausted";
    case SCHEDULER_STATUS_DESCRIPTOR_LIMIT:
        return "dynamic task descriptor limit is exhausted";
    case SCHEDULER_STATUS_QUEUE_LIMIT:
        return "ready queue limit is exhausted";
    case SCHEDULER_STATUS_NO_RUNNABLE_PEER:
        return "no runnable peer is available";
    case SCHEDULER_STATUS_INVALID_STATE:
        return "task state transition is invalid";
    case SCHEDULER_STATUS_ARITHMETIC_OVERFLOW:
        return "scheduler address arithmetic overflowed";
    case SCHEDULER_STATUS_OUT_OF_MEMORY:
        return "physical memory for a task stack is exhausted";
    case SCHEDULER_STATUS_FRAME_FAILURE:
        return "task-stack physical-frame operation failed";
    case SCHEDULER_STATUS_MAPPING_FAILURE:
        return "task-stack mapping operation failed";
    case SCHEDULER_STATUS_CONTEXT_FAILURE:
        return "saved task context or stack pointer is invalid";
    case SCHEDULER_STATUS_ROLLBACK_FAILURE:
        return "scheduler rollback failed and scheduling is poisoned";
    case SCHEDULER_STATUS_RUNNING_TASK:
        return "a running task cannot be reaped";
    case SCHEDULER_STATUS_RUNNABLE_TASK:
        return "a ready task cannot be reaped";
    case SCHEDULER_STATUS_DOUBLE_REAP:
        return "task was already reaped or retired";
    case SCHEDULER_STATUS_BOOTSTRAP_TASK:
        return "bootstrap task cannot use the dynamic-task exit path";
    case SCHEDULER_STATUS_STATS_INVALID:
        return "scheduler statistics are inconsistent";
    case SCHEDULER_STATUS_VALIDATION_FAILURE:
        return "scheduler validation failed";
    case SCHEDULER_STATUS_TEST_FAILURE:
        return "injected scheduler transaction failure";
    default:
        return "unknown scheduler status";
    }
}

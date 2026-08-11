/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/heap.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/preempt.h>
#include <zenith/scheduler.h>
#include <zenith/spinlock.h>
#include <zenith/virtual_memory.h>
#include <zenith/xstate.h>

#include "scheduler_arch.h"
#include "scheduler_core.h"
#include "xstate_runtime.h"

#define SCHEDULER_STACK_PAGE_COUNT VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES
#define SCHEDULER_STACK_SIZE \
    ((uint64_t)SCHEDULER_STACK_PAGE_COUNT * ZENITH_PAGE_SIZE)
#define SCHEDULER_RFLAGS_RESERVED UINT64_C(0x2)
#define SCHEDULER_RFLAGS_IF UINT64_C(0x200)
#define SCHEDULER_RFLAGS_TF UINT64_C(0x100)
#define SCHEDULER_RFLAGS_RF UINT64_C(0x10000)
#define SCHEDULER_RFLAGS_ALLOWED UINT64_C(0x240ED7)
#define SCHEDULER_LOCK_INDEX UINT32_C(1)
#define SCHEDULER_STACK_CANARY_SIZE ((size_t)64U)
#define SCHEDULER_STACK_CANARY_BYTE UINT8_C(0x3C)
#define SCHEDULER_STACK_UNUSED_BYTE UINT8_C(0xA5)

struct scheduler_runtime_task {
    struct interrupt_frame *saved_frame;
    scheduler_task_entry_t entry;
    void *opaque_context;
    uintptr_t frames[SCHEDULER_STACK_PAGE_COUNT];
    uint64_t lower_guard;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper_guard;
    uint64_t generation;
    struct xstate_task_handle xstate_handle;
    enum scheduler_status resume_status;
    bool populated;
    bool xstate_populated;
    bool first_entry_started;
    bool saved_frame_synthetic;
};

enum scheduler_switch_request {
    SCHEDULER_SWITCH_NONE = 0,
    SCHEDULER_SWITCH_YIELD,
    SCHEDULER_SWITCH_BLOCK,
    SCHEDULER_SWITCH_JOIN,
    SCHEDULER_SWITCH_SLEEP_UNTIL,
    SCHEDULER_SWITCH_SLEEP_FOR,
    SCHEDULER_SWITCH_CANCEL,
    SCHEDULER_SWITCH_EXIT
};

extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t bootstrap_stack_bottom[];
extern uint8_t bootstrap_stack_top[];

static struct scheduler_core_state active_core;
static struct scheduler_core_state candidate_core;
static struct scheduler_core_state self_test_core;
static struct scheduler_runtime_task runtime_tasks[SCHEDULER_TASK_LIMIT];
static struct spinlock scheduler_lock;
static struct interrupt_frame *bootstrap_saved_frame;
static enum scheduler_status bootstrap_resume_status;
static uintptr_t transaction_frames[SCHEDULER_STACK_PAGE_COUNT];
static bool transaction_mapped[SCHEDULER_STACK_PAGE_COUNT];
static bool scheduler_initialized;
static bool scheduler_poisoned;
static volatile enum scheduler_switch_request pending_switch;
static struct scheduler_core_event_token pending_event;
static struct scheduler_task_handle pending_join_target;
static uint64_t pending_sleep_value;
static uint32_t quantum_ticks;
static size_t timer_reschedule_requests;
static size_t preemption_count;
static struct frame_allocator_task_stack_certificate frame_certificate;
static struct virtual_memory_task_stack_certificate vm_certificate;
static bool resource_certificate_valid;

_Static_assert(sizeof(struct scheduler_core_state) == 1440U,
    "scheduler core BSS accounting changed");
_Static_assert(sizeof(struct scheduler_runtime_task) == 208U,
    "scheduler runtime-record BSS accounting changed");
_Static_assert(sizeof(scheduler_task_entry_t) == sizeof(uintptr_t),
    "scheduler entry pointers must fit in uintptr_t");
_Static_assert(INTERRUPT_KERNEL_FRAME_SIZE == 184U,
    "ring-zero resumable interrupt-frame size changed");
_Static_assert(SCHEDULER_TASK_LIMIT == VIRTUAL_MEMORY_TASK_STACK_LIMIT,
    "scheduler descriptor and stack-slot limits disagree");
_Static_assert(SCHEDULER_TASK_LIMIT == (size_t)XSTATE_TASK_LIMIT,
    "scheduler and eager-XSAVE task limits disagree");
_Static_assert(SCHEDULER_EVENT_LIMIT == SCHEDULER_CORE_EVENT_LIMIT,
    "scheduler public and core event limits disagree");
_Static_assert(SCHEDULER_STACK_SIZE == UINT64_C(65536),
    "dynamic task stack payload must remain 64 KiB");
_Static_assert(SCHEDULER_STACK_CANARY_SIZE < SCHEDULER_STACK_SIZE,
    "stack canary must leave usable task stack space");
_Static_assert(SCHEDULER_QUANTUM_TICKS > 0U,
    "scheduler quantum must contain at least one timer tick");

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static void resource_certificate_clear(void)
{
    bytes_zero(&frame_certificate, sizeof(frame_certificate));
    bytes_zero(&vm_certificate, sizeof(vm_certificate));
    resource_certificate_valid = false;
}

static bool resource_certificate_capture(void)
{
    struct frame_allocator_task_stack_certificate observed_frame;
    struct virtual_memory_task_stack_certificate observed_vm;

    if (cpu_interrupts_enabled() ||
        frame_allocator_task_stack_certificate_get(&observed_frame) !=
            FRAME_STATUS_OK ||
        virtual_memory_task_stack_certificate_get(&observed_vm) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        observed_frame.mutation_epoch == 0U ||
        observed_vm.mutation_epoch == 0U ||
        observed_frame.owned_frames != observed_vm.mapped_pages) {
        resource_certificate_clear();
        return false;
    }

    frame_certificate = observed_frame;
    vm_certificate = observed_vm;
    resource_certificate_valid = true;
    return true;
}

static bool resource_certificate_matches(void)
{
    struct frame_allocator_task_stack_certificate observed_frame;
    struct virtual_memory_task_stack_certificate observed_vm;

    return resource_certificate_valid && !cpu_interrupts_enabled() &&
        frame_allocator_task_stack_certificate_get(&observed_frame) ==
            FRAME_STATUS_OK &&
        virtual_memory_task_stack_certificate_get(&observed_vm) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        observed_frame.mutation_epoch == frame_certificate.mutation_epoch &&
        observed_frame.owned_frames == frame_certificate.owned_frames &&
        observed_vm.mutation_epoch == vm_certificate.mutation_epoch &&
        observed_vm.mapped_pages == vm_certificate.mapped_pages &&
        observed_frame.owned_frames == observed_vm.mapped_pages;
}

static bool scheduler_switch_is_allowed(void)
{
    bool allowed = false;

    return preempt_runtime_switch_allowed(&allowed) == PREEMPT_STATUS_OK &&
        allowed;
}

static bool quantum_ticks_are_valid(uint32_t ticks)
{
    return ticks < SCHEDULER_QUANTUM_TICKS;
}

static bool scheduler_lock_enter(struct spinlock_irq_token *token)
{
    return token != NULL &&
        spinlock_irqsave_acquire(&scheduler_lock, token) ==
            SPINLOCK_STATUS_OK;
}

static bool scheduler_lock_leave(struct spinlock_irq_token *token)
{
    return token != NULL &&
        spinlock_irqrestore_release(&scheduler_lock, token) ==
            SPINLOCK_STATUS_OK;
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
    destination->saved_frame = source->saved_frame;
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
    destination->xstate_handle = source->xstate_handle;
    destination->resume_status = source->resume_status;
    destination->populated = source->populated;
    destination->xstate_populated = source->xstate_populated;
    destination->first_entry_started = source->first_entry_started;
    destination->saved_frame_synthetic = source->saved_frame_synthetic;
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

static bool saved_frame_is_valid(
    const struct interrupt_frame *frame,
    uintptr_t stack_start,
    uintptr_t stack_end,
    bool synthetic
)
{
    const uintptr_t first_entry =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;

    return frame != NULL && stack_start < stack_end &&
        address_is_canonical((uint64_t)(uintptr_t)(const void *)frame) &&
        (uintptr_t)(const void *)frame >= stack_start &&
        (uintptr_t)(const void *)frame <=
            stack_end - INTERRUPT_KERNEL_FRAME_SIZE &&
        ((uintptr_t)(const void *)frame & (sizeof(uint64_t) - 1U)) == 0U &&
        frame->vector < INTERRUPT_VECTOR_COUNT &&
        frame->cs == CPU_GDT_CODE_SELECTOR &&
        frame->ss == CPU_GDT_DATA_SELECTOR &&
        address_is_canonical(frame->rsp) &&
        frame->rsp >= stack_start && frame->rsp <= stack_end &&
        (frame->rflags & SCHEDULER_RFLAGS_RESERVED) != 0U &&
        (frame->rflags & ~SCHEDULER_RFLAGS_ALLOWED) == 0U &&
        kernel_text_address((uintptr_t)frame->rip) &&
        (!synthetic ||
            (frame->rip == first_entry &&
             frame->vector == INTERRUPT_RESCHEDULE_VECTOR &&
             frame->error_code == 0U));
}

static bool prepare_interrupt_rflags_for_resume(uint64_t *rflags)
{
    uint64_t candidate;

    if (rflags == NULL) {
        return false;
    }

    /*
     * Hardware normally sets RF in an exception's stacked RFLAGS image.  RF
     * is entry metadata, not task state: discard it before the frame can become
     * a saved continuation.  Every other disallowed bit, including TF, fails
     * closed and leaves the caller's frame unchanged.
     */
    candidate = *rflags & ~SCHEDULER_RFLAGS_RF;
    if ((candidate & SCHEDULER_RFLAGS_RESERVED) == 0U ||
        (candidate & ~SCHEDULER_RFLAGS_ALLOWED) != 0U) {
        return false;
    }

    *rflags = candidate;
    return true;
}

static bool running_stack_is_valid(uintptr_t stack_start, uintptr_t stack_end)
{
    const uintptr_t stack_pointer = cpu_read_stack_pointer();

    return stack_start < stack_end &&
        address_is_canonical((uint64_t)stack_pointer) &&
        stack_pointer >= stack_start && stack_pointer < stack_end &&
        (stack_pointer & (sizeof(uintptr_t) - 1U)) == 0U;
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
    case SCHEDULER_CORE_INVALID_PRIORITY:
        return SCHEDULER_STATUS_INVALID_PRIORITY;
    case SCHEDULER_CORE_INVALID_EVENT:
        return SCHEDULER_STATUS_INVALID_EVENT;
    case SCHEDULER_CORE_EVENT_CHANGED:
        return SCHEDULER_STATUS_EVENT_CHANGED;
    case SCHEDULER_CORE_EVENT_EPOCH_EXHAUSTED:
        return SCHEDULER_STATUS_EVENT_EPOCH_EXHAUSTED;
    case SCHEDULER_CORE_BLOCKED_TASK:
        return SCHEDULER_STATUS_BLOCKED_TASK;
    case SCHEDULER_CORE_SELF_JOIN:
        return SCHEDULER_STATUS_SELF_JOIN;
    case SCHEDULER_CORE_JOIN_CYCLE:
        return SCHEDULER_STATUS_JOIN_CYCLE;
    case SCHEDULER_CORE_JOIN_COMPLETION_PENDING:
        return SCHEDULER_STATUS_JOIN_COMPLETION_PENDING;
    case SCHEDULER_CORE_CANCELLATION_NOT_REQUESTED:
        return SCHEDULER_STATUS_CANCELLATION_NOT_REQUESTED;
    case SCHEDULER_CORE_INVALID_DEADLINE:
        return SCHEDULER_STATUS_INVALID_DEADLINE;
    case SCHEDULER_CORE_TIME_REGRESSION:
        return SCHEDULER_STATUS_TIME_REGRESSION;
    case SCHEDULER_CORE_TICK_EXHAUSTED:
        return SCHEDULER_STATUS_TICK_EXHAUSTED;
    case SCHEDULER_CORE_DEADLINE_OVERFLOW:
        return SCHEDULER_STATUS_DEADLINE_OVERFLOW;
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

static void set_current_resume_status(enum scheduler_status status)
{
    if (active_core.current.bootstrap) {
        bootstrap_resume_status = status;
    } else {
        runtime_tasks[active_core.current.index].resume_status = status;
    }
}

static bool stack_canary_is_intact(const struct scheduler_runtime_task *task)
{
    const volatile uint8_t *bytes;

    if (task == NULL || !task->populated ||
        task->stack_end - task->stack_start != SCHEDULER_STACK_SIZE) {
        return false;
    }

    bytes = (const volatile uint8_t *)(uintptr_t)task->stack_start;
    for (size_t index = 0U; index < SCHEDULER_STACK_CANARY_SIZE; ++index) {
        if (bytes[index] != SCHEDULER_STACK_CANARY_BYTE) {
            return false;
        }
    }
    return true;
}

static bool stack_high_water_measure(
    const struct scheduler_runtime_task *task,
    size_t *high_water_bytes
)
{
    const volatile uint8_t *bytes;
    size_t first_used = SCHEDULER_STACK_CANARY_SIZE;

    if (high_water_bytes == NULL || !stack_canary_is_intact(task)) {
        return false;
    }

    bytes = (const volatile uint8_t *)(uintptr_t)task->stack_start;
    while (first_used < (size_t)SCHEDULER_STACK_SIZE &&
           bytes[first_used] == SCHEDULER_STACK_UNUSED_BYTE) {
        ++first_used;
    }
    *high_water_bytes = (size_t)SCHEDULER_STACK_SIZE - first_used;
    return true;
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
        !guards_absent(task_index) || !stack_canary_is_intact(task)) {
        return false;
    }

    switch (core->descriptors[task_index].state) {
    case SCHEDULER_TASK_RUNNING:
        if (core->current.bootstrap || core->current.index != task_index ||
            core->current.generation != task->generation ||
            task->saved_frame != NULL || task->saved_frame_synthetic ||
            !running_stack_is_valid(
                (uintptr_t)stack_start,
                (uintptr_t)stack_end
            )) {
            return false;
        }
        if (!task->xstate_populated ||
            xstate_runtime_task_validate(task->xstate_handle, true) !=
                XSTATE_STATUS_OK) {
            return false;
        }
        break;
    case SCHEDULER_TASK_CONSTRUCTING:
        if (task->xstate_populated ||
            !saved_frame_is_valid(
                task->saved_frame,
                (uintptr_t)stack_start,
                (uintptr_t)stack_end,
                task->saved_frame_synthetic
            )) {
            return false;
        }
        break;
    case SCHEDULER_TASK_READY:
    case SCHEDULER_TASK_BLOCKED:
    case SCHEDULER_TASK_JOIN_BLOCKED:
    case SCHEDULER_TASK_SLEEPING:
        if (!saved_frame_is_valid(
                task->saved_frame,
                (uintptr_t)stack_start,
                (uintptr_t)stack_end,
                task->saved_frame_synthetic
            )) {
            return false;
        }
        if (!task->xstate_populated ||
            xstate_runtime_task_validate(task->xstate_handle, false) !=
                XSTATE_STATUS_OK) {
            return false;
        }
        break;
    case SCHEDULER_TASK_EXITED:
    case SCHEDULER_TASK_REAPING:
        if (task->saved_frame != NULL || task->saved_frame_synthetic) {
            return false;
        }
        if (!task->xstate_populated ||
            xstate_runtime_task_validate(task->xstate_handle, false) !=
                XSTATE_STATUS_OK) {
            return false;
        }
        break;
    default:
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

/*
 * Timer IRQs may arrive back-to-back at 1 kHz.  They cannot afford the full
 * allocator/VM/heap/xstate audit performed at task and public transaction
 * boundaries.  Time advancement mutates only scheduler-core wait/ready
 * metadata, so validate that core plus the fixed runtime metadata and saved
 * frame provenance that must remain coherent with it.
 */
static bool timer_runtime_metadata_is_valid(
    const struct scheduler_core_state *core,
    size_t *live_task_count
)
{
    if (live_task_count == NULL) {
        return false;
    }
    *live_task_count = 0U;

    if ((core->bootstrap_state == SCHEDULER_TASK_RUNNING &&
            (bootstrap_saved_frame != NULL ||
             !running_stack_is_valid(
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top
             ))) ||
        (core->bootstrap_state == SCHEDULER_TASK_READY &&
            !saved_frame_is_valid(
                bootstrap_saved_frame,
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top,
                false
            ))) {
        return false;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const struct scheduler_core_descriptor *descriptor =
            &core->descriptors[index];
        const struct scheduler_runtime_task *task = &runtime_tasks[index];
        uint64_t lower;
        uint64_t stack_start;
        uint64_t stack_end;
        uint64_t upper;
        union {
            scheduler_task_entry_t function;
            uintptr_t address;
        } entry;

        if (descriptor->state == SCHEDULER_TASK_UNUSED ||
            descriptor->state == SCHEDULER_TASK_RETIRED) {
            if (!runtime_task_is_clear(task)) {
                return false;
            }
            continue;
        }

        ++*live_task_count;

        entry.function = task->entry;
        if (!task->populated || !task->xstate_populated ||
            task->generation == 0U ||
            task->generation != descriptor->generation ||
            task->xstate_handle.index >= XSTATE_TASK_LIMIT ||
            task->xstate_handle.generation == 0U ||
            task->entry == NULL || !kernel_text_address(entry.address) ||
            virtual_memory_task_stack_bounds(
                index,
                &lower,
                &stack_start,
                &stack_end,
                &upper
            ) != VIRTUAL_MEMORY_STATUS_OK ||
            task->lower_guard != lower || task->stack_start != stack_start ||
            task->stack_end != stack_end || task->upper_guard != upper ||
            stack_end - stack_start != SCHEDULER_STACK_SIZE ||
            !stack_canary_is_intact(task)) {
            return false;
        }

        switch (descriptor->state) {
        case SCHEDULER_TASK_RUNNING:
            if (core->current.bootstrap || core->current.index != index ||
                core->current.generation != task->generation ||
                task->saved_frame != NULL || task->saved_frame_synthetic ||
                !running_stack_is_valid(
                    (uintptr_t)stack_start,
                    (uintptr_t)stack_end
                )) {
                return false;
            }
            break;
        case SCHEDULER_TASK_READY:
        case SCHEDULER_TASK_BLOCKED:
        case SCHEDULER_TASK_JOIN_BLOCKED:
        case SCHEDULER_TASK_SLEEPING:
            if (!saved_frame_is_valid(
                    task->saved_frame,
                    (uintptr_t)stack_start,
                    (uintptr_t)stack_end,
                    task->saved_frame_synthetic
                )) {
                return false;
            }
            break;
        case SCHEDULER_TASK_EXITED:
            if (task->saved_frame != NULL || task->saved_frame_synthetic) {
                return false;
            }
            break;
        case SCHEDULER_TASK_UNUSED:
        case SCHEDULER_TASK_CONSTRUCTING:
        case SCHEDULER_TASK_REAPING:
        case SCHEDULER_TASK_RETIRED:
        case SCHEDULER_TASK_POISONED:
        default:
            return false;
        }
    }

    return true;
}

static enum scheduler_status validate_local_state(
    const struct scheduler_core_state *core
)
{
    struct xstate_runtime_snapshot xstate_snapshot;
    size_t live_task_count;
    uint32_t expected_loaded = XSTATE_INVALID_INDEX;
    uint64_t expected_generation = 0U;
    enum scheduler_core_status core_status;

    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (scheduler_poisoned || core->poisoned) {
        return SCHEDULER_STATUS_POISONED;
    }
    if (!quantum_ticks_are_valid(quantum_ticks)) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    core_status = scheduler_core_validate(core);
    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }
    if (!resource_certificate_matches() ||
        !timer_runtime_metadata_is_valid(core, &live_task_count) ||
        live_task_count > SIZE_MAX / SCHEDULER_STACK_PAGE_COUNT ||
        frame_certificate.owned_frames !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT ||
        vm_certificate.mapped_pages !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    if (!core->current.bootstrap) {
        expected_loaded =
            runtime_tasks[core->current.index].xstate_handle.index;
        expected_generation =
            runtime_tasks[core->current.index].xstate_handle.generation;
    }
    if (xstate_runtime_snapshot(&xstate_snapshot) != XSTATE_STATUS_OK ||
        !xstate_snapshot.initialized || xstate_snapshot.poisoned ||
        xstate_snapshot.live_tasks != live_task_count ||
        xstate_snapshot.bootstrap_loaded != core->current.bootstrap ||
        xstate_snapshot.loaded_task != expected_loaded ||
        xstate_snapshot.loaded_generation != expected_generation) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status validate_state_internal(
    const struct scheduler_core_state *core,
    bool include_heap
)
{
    struct frame_allocator_task_stack_certificate observed_frame;
    struct virtual_memory_task_stack_certificate observed_vm;
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

    if ((include_heap
            ? heap_validate() != HEAP_STATUS_OK
            : (frame_allocator_validate() != FRAME_STATUS_OK ||
               virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK)) ||
        frame_allocator_task_stack_certificate_get(&observed_frame) !=
            FRAME_STATUS_OK ||
        virtual_memory_task_stack_certificate_get(&observed_vm) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        observed_frame.mutation_epoch == 0U ||
        observed_vm.mutation_epoch == 0U) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    if ((core->bootstrap_state == SCHEDULER_TASK_RUNNING &&
            (bootstrap_saved_frame != NULL ||
             !running_stack_is_valid(
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top
            ))) ||
        (core->bootstrap_state == SCHEDULER_TASK_READY &&
            !saved_frame_is_valid(
                    bootstrap_saved_frame,
                    (uintptr_t)(void *)bootstrap_stack_bottom,
                    (uintptr_t)(void *)bootstrap_stack_top,
                    false
                ))) {
        return SCHEDULER_STATUS_CONTEXT_FAILURE;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const enum scheduler_task_state state =
            core->descriptors[index].state;

        switch (state) {
        case SCHEDULER_TASK_READY:
        case SCHEDULER_TASK_RUNNING:
        case SCHEDULER_TASK_BLOCKED:
        case SCHEDULER_TASK_JOIN_BLOCKED:
        case SCHEDULER_TASK_SLEEPING:
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
            /*
             * The transitive VM audit above proves every guard and leaf.
             * Exact live-task mappings plus the mapped-page equality below
             * exclude any payload mapping in an inactive slot.
             */
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
        observed_vm.mapped_pages !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT ||
        observed_frame.owned_frames !=
            live_task_count * SCHEDULER_STACK_PAGE_COUNT ||
        observed_frame.owned_frames != observed_vm.mapped_pages) {
        return SCHEDULER_STATUS_STATS_INVALID;
    }

    {
        struct xstate_runtime_snapshot xstate_snapshot;
        uint32_t expected_loaded = XSTATE_INVALID_INDEX;

        if (!core->current.bootstrap) {
            expected_loaded =
                runtime_tasks[core->current.index].xstate_handle.index;
        }
        if (xstate_runtime_snapshot(&xstate_snapshot) != XSTATE_STATUS_OK ||
            xstate_snapshot.live_tasks != live_task_count ||
            xstate_snapshot.bootstrap_loaded != core->current.bootstrap ||
            xstate_snapshot.loaded_task != expected_loaded) {
            return SCHEDULER_STATUS_VALIDATION_FAILURE;
        }
    }

    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status validate_state(
    const struct scheduler_core_state *core
)
{
    return validate_state_internal(core, true);
}

static size_t active_live_task_count(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        switch (active_core.descriptors[index].state) {
        case SCHEDULER_TASK_READY:
        case SCHEDULER_TASK_RUNNING:
        case SCHEDULER_TASK_BLOCKED:
        case SCHEDULER_TASK_JOIN_BLOCKED:
        case SCHEDULER_TASK_SLEEPING:
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

static bool mapped_stack_has_initial_pattern(
    const struct scheduler_runtime_task *task
)
{
    const volatile uint8_t *bytes =
        (const volatile uint8_t *)(uintptr_t)task->stack_start;

    for (size_t index = 0U; index < (size_t)SCHEDULER_STACK_SIZE; ++index) {
        const uint8_t expected = index < SCHEDULER_STACK_CANARY_SIZE
            ? SCHEDULER_STACK_CANARY_BYTE
            : SCHEDULER_STACK_UNUSED_BYTE;

        if (bytes[index] != expected) {
            return false;
        }
    }

    return true;
}

static enum scheduler_status validate_construction(
    struct scheduler_task_handle handle
)
{
    struct frame_allocator_task_stack_certificate observed_frame;
    struct virtual_memory_task_stack_certificate observed_vm;
    size_t previous_live_tasks = active_live_task_count();

    if (handle.index >= SCHEDULER_TASK_LIMIT ||
        candidate_core.descriptors[handle.index].state !=
            SCHEDULER_TASK_CONSTRUCTING ||
        heap_validate() != HEAP_STATUS_OK ||
        frame_allocator_task_stack_certificate_get(&observed_frame) !=
            FRAME_STATUS_OK ||
        virtual_memory_task_stack_certificate_get(&observed_vm) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        observed_frame.mutation_epoch == 0U ||
        observed_vm.mutation_epoch == 0U ||
        previous_live_tasks >= SCHEDULER_TASK_LIMIT ||
        observed_vm.mapped_pages !=
            (previous_live_tasks + 1U) * SCHEDULER_STACK_PAGE_COUNT ||
        observed_frame.owned_frames != observed_vm.mapped_pages ||
        observed_frame.owned_frames !=
            (previous_live_tasks + 1U) * SCHEDULER_STACK_PAGE_COUNT ||
        !runtime_task_is_valid(handle.index, &candidate_core)) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    return SCHEDULER_STATUS_OK;
}

static bool target_frame_is_valid(
    const struct scheduler_core_identity *identity
)
{
    if (identity->bootstrap) {
        return saved_frame_is_valid(
                bootstrap_saved_frame,
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top,
                false
            );
    }

    if (identity->index >= SCHEDULER_TASK_LIMIT ||
        runtime_tasks[identity->index].generation != identity->generation) {
        return false;
    }

    return saved_frame_is_valid(
        runtime_tasks[identity->index].saved_frame,
        (uintptr_t)runtime_tasks[identity->index].stack_start,
        (uintptr_t)runtime_tasks[identity->index].stack_end,
        runtime_tasks[identity->index].saved_frame_synthetic
    );
}

static bool identities_equal(
    const struct scheduler_core_identity *left,
    const struct scheduler_core_identity *right
)
{
    return left->bootstrap == right->bootstrap &&
        (left->bootstrap ||
            (left->index == right->index &&
             left->generation == right->generation));
}

bool scheduler_interrupt_return_frame_is_valid(
    const struct interrupt_frame *frame
)
{
    if (!scheduler_initialized || scheduler_poisoned || frame == NULL) {
        return false;
    }

    if (active_core.current.bootstrap) {
        return active_core.bootstrap_state == SCHEDULER_TASK_RUNNING &&
            saved_frame_is_valid(
                frame,
                (uintptr_t)(void *)bootstrap_stack_bottom,
                (uintptr_t)(void *)bootstrap_stack_top,
                false
            );
    }

    if (active_core.current.index >= SCHEDULER_TASK_LIMIT ||
        active_core.descriptors[active_core.current.index].state !=
            SCHEDULER_TASK_RUNNING ||
        runtime_tasks[active_core.current.index].generation !=
            active_core.current.generation) {
        return false;
    }

    return saved_frame_is_valid(
        frame,
        (uintptr_t)runtime_tasks[active_core.current.index].stack_start,
        (uintptr_t)runtime_tasks[active_core.current.index].stack_end,
        false
    );
}

static bool vector_uses_ist(uint8_t vector)
{
    return vector == 2U || vector == 8U || vector == 18U ||
        vector == INTERRUPT_IST_TEST_VECTOR;
}

void scheduler_timer_tick(void)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_core_status core_status;
    size_t woken_count = 0U;

    if (!scheduler_initialized || scheduler_poisoned) {
        return;
    }

    if (!scheduler_lock_enter(&lock_token)) {
        poison_scheduler();
        return;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_timer_tick(&candidate_core, &woken_count);
    if (core_status != SCHEDULER_CORE_OK ||
        validate_local_state(&candidate_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
    } else {
        scheduler_core_copy(&active_core, &candidate_core);

        if (active_core.queue_count == 0U) {
            quantum_ticks = 0U;
        } else if (quantum_ticks + 1U < SCHEDULER_QUANTUM_TICKS) {
            ++quantum_ticks;
        } else {
            quantum_ticks = 0U;
            if (timer_reschedule_requests == SIZE_MAX ||
                preempt_runtime_request_reschedule() != PREEMPT_STATUS_OK) {
                poison_scheduler();
            } else {
                ++timer_reschedule_requests;
            }
        }
    }

    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
    }
}

struct interrupt_frame *scheduler_interrupt_exit(
    struct interrupt_frame *frame,
    uint8_t vector,
    bool reschedule_eligible
)
{
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    struct interrupt_frame *next_frame;
    struct spinlock_irq_token lock_token;
    enum scheduler_switch_request request = SCHEDULER_SWITCH_NONE;
    enum scheduler_core_status core_status;
    bool involuntary = false;
    bool join_completed = false;
    bool cancelled = false;
    bool terminal_switch = false;

    if (!scheduler_initialized || scheduler_poisoned || frame == NULL ||
        vector_uses_ist(vector)) {
        return frame;
    }

    if (vector == INTERRUPT_RESCHEDULE_VECTOR &&
        pending_switch != SCHEDULER_SWITCH_NONE) {
        request = pending_switch;
    } else if (reschedule_eligible) {
        request = SCHEDULER_SWITCH_YIELD;
        involuntary = true;
    } else {
        /* Ordinary interrupt return never touches the scheduler lock. */
        return frame;
    }

    /* irq_exit proved eligibility before this lock adds its preempt pin. */
    if (reschedule_eligible &&
        preempt_runtime_take_reschedule() != PREEMPT_STATUS_OK) {
        poison_scheduler();
        console_panic("scheduler could not consume reschedule request");
    }

    if (!scheduler_lock_enter(&lock_token)) {
        poison_scheduler();
        console_panic("scheduler interrupt lock acquisition failed");
    }

    if (!prepare_interrupt_rflags_for_resume(&frame->rflags) ||
        !scheduler_interrupt_return_frame_is_valid(frame)) {
        poison_scheduler();
        console_panic("scheduler rejected interrupt-frame provenance");
    }

    scheduler_core_copy(&candidate_core, &active_core);
    switch (request) {
    case SCHEDULER_SWITCH_YIELD:
        core_status = scheduler_core_yield(
            &candidate_core,
            &previous,
            &next
        );
        break;
    case SCHEDULER_SWITCH_BLOCK:
        core_status = scheduler_core_block_current(
            &candidate_core,
            pending_event,
            &previous,
            &next
        );
        break;
    case SCHEDULER_SWITCH_JOIN:
        core_status = scheduler_core_join_current(
            &candidate_core,
            pending_join_target,
            &join_completed,
            &previous,
            &next
        );
        break;
    case SCHEDULER_SWITCH_SLEEP_UNTIL:
        core_status = scheduler_core_sleep_current_until(
            &candidate_core,
            pending_sleep_value,
            &previous,
            &next
        );
        break;
    case SCHEDULER_SWITCH_SLEEP_FOR:
        core_status = scheduler_core_sleep_current_for(
            &candidate_core,
            pending_sleep_value,
            &previous,
            &next
        );
        break;
    case SCHEDULER_SWITCH_CANCEL:
        core_status = scheduler_core_cancellation_point(
            &candidate_core,
            &cancelled,
            &previous,
            &next
        );
        terminal_switch = cancelled;
        break;
    case SCHEDULER_SWITCH_EXIT:
        core_status = scheduler_core_exit_current(
            &candidate_core,
            &previous,
            &next
        );
        terminal_switch = core_status == SCHEDULER_CORE_OK;
        break;
    case SCHEDULER_SWITCH_NONE:
    default:
        core_status = SCHEDULER_CORE_CORRUPTED;
        break;
    }

    if (core_status != SCHEDULER_CORE_OK) {
        if (request == SCHEDULER_SWITCH_EXIT) {
            poison_scheduler();
            console_panic("exiting task has no safe runnable peer");
        }

        if (!involuntary) {
            if (active_core.current.bootstrap) {
                bootstrap_resume_status = status_from_core(core_status);
            } else {
                runtime_tasks[active_core.current.index].resume_status =
                    status_from_core(core_status);
            }
            pending_switch = SCHEDULER_SWITCH_NONE;
        }
        if (!scheduler_lock_leave(&lock_token)) {
            poison_scheduler();
            console_panic("scheduler interrupt lock release failed");
        }
        return frame;
    }

    if (join_completed) {
        scheduler_core_copy(&active_core, &candidate_core);
        set_current_resume_status(SCHEDULER_STATUS_OK);
        pending_switch = SCHEDULER_SWITCH_NONE;
        if (!scheduler_lock_leave(&lock_token)) {
            poison_scheduler();
            console_panic("scheduler interrupt lock release failed");
        }
        return frame;
    }

    if (identities_equal(&previous, &next)) {
        quantum_ticks = 0U;
        scheduler_core_copy(&active_core, &candidate_core);
        if (!involuntary) {
            if (previous.bootstrap) {
                bootstrap_resume_status = SCHEDULER_STATUS_OK;
            } else {
                runtime_tasks[previous.index].resume_status =
                    SCHEDULER_STATUS_OK;
            }
            pending_switch = SCHEDULER_SWITCH_NONE;
        }
        if (!scheduler_lock_leave(&lock_token)) {
            poison_scheduler();
            console_panic("scheduler interrupt lock release failed");
        }
        return frame;
    }

    if (!target_frame_is_valid(&next)) {
        poison_scheduler();
        console_panic("scheduler selected an unowned return frame");
    }

    next_frame = next.bootstrap
        ? bootstrap_saved_frame
        : runtime_tasks[next.index].saved_frame;

    if (cpu_interrupts_enabled() ||
        xstate_runtime_switch(
            previous.bootstrap,
            previous.bootstrap
                ? (struct xstate_task_handle){XSTATE_INVALID_INDEX, 0U}
                : runtime_tasks[previous.index].xstate_handle,
            next.bootstrap,
            next.bootstrap
                ? (struct xstate_task_handle){XSTATE_INVALID_INDEX, 0U}
                : runtime_tasks[next.index].xstate_handle
        ) != XSTATE_STATUS_OK) {
        poison_scheduler();
        console_panic("scheduler eager XSAVE transition failed");
    }

    if (previous.bootstrap) {
        bootstrap_saved_frame = frame;
    } else if (terminal_switch) {
        runtime_tasks[previous.index].saved_frame = NULL;
        runtime_tasks[previous.index].saved_frame_synthetic = false;
    } else if (!terminal_switch) {
        runtime_tasks[previous.index].saved_frame = frame;
        runtime_tasks[previous.index].saved_frame_synthetic = false;
    }

    if (next.bootstrap) {
        bootstrap_saved_frame = NULL;
    } else {
        runtime_tasks[next.index].saved_frame = NULL;
        runtime_tasks[next.index].saved_frame_synthetic = false;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    quantum_ticks = 0U;
    if (involuntary) {
        if (preemption_count == SIZE_MAX) {
            poison_scheduler();
            console_panic("scheduler preemption counter exhausted");
        }
        ++preemption_count;
    } else {
        if (previous.bootstrap) {
            bootstrap_resume_status = SCHEDULER_STATUS_OK;
        } else {
            runtime_tasks[previous.index].resume_status = SCHEDULER_STATUS_OK;
        }
        pending_switch = SCHEDULER_SWITCH_NONE;
    }

    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        console_panic("scheduler interrupt lock release failed");
    }

    return next_frame;
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

    if (runtime_tasks[handle.index].xstate_populated &&
        xstate_runtime_task_destroy(
            runtime_tasks[handle.index].xstate_handle
        ) != XSTATE_STATUS_OK) {
        rollback_failed = true;
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
    if (!resource_certificate_capture()) {
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
    }
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

    if (!mapped_stack_is_zero(task)) {
        enum scheduler_status rollback_status = rollback_creation(
            handle,
            allocated_count
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : rollback_status;
    }

    {
        volatile uint8_t *stack_bytes =
            (volatile uint8_t *)(uintptr_t)task->stack_start;

        for (size_t index = 0U;
             index < (size_t)SCHEDULER_STACK_SIZE;
             ++index) {
            stack_bytes[index] = index < SCHEDULER_STACK_CANARY_SIZE
                ? SCHEDULER_STACK_CANARY_BYTE
                : SCHEDULER_STACK_UNUSED_BYTE;
        }
    }

    task->entry = entry;
    task->opaque_context = opaque_context;
    task->generation = handle.generation;
    task->populated = true;

    for (size_t page = 0U; page < SCHEDULER_STACK_PAGE_COUNT; ++page) {
        task->frames[page] = transaction_frames[page];
    }

    if (!mapped_stack_has_initial_pattern(task)) {
        enum scheduler_status rollback_status = rollback_creation(
            handle,
            allocated_count
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : rollback_status;
    }

    task->saved_frame = (struct interrupt_frame *)(uintptr_t)(
        task->stack_end - sizeof(struct interrupt_frame)
    );
    bytes_zero(task->saved_frame, sizeof(*task->saved_frame));
    task->saved_frame->vector = INTERRUPT_RESCHEDULE_VECTOR;
    task->saved_frame->rip =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;
    task->saved_frame->cs = CPU_GDT_CODE_SELECTOR;
    task->saved_frame->rflags = SCHEDULER_RFLAGS_RESERVED;
    task->saved_frame->rsp = task->stack_end;
    task->saved_frame->ss = CPU_GDT_DATA_SELECTOR;
    task->saved_frame_synthetic = true;

    if (!saved_frame_is_valid(
            task->saved_frame,
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
    _Alignas(16) uintptr_t fake_stack[24];
    volatile uintptr_t *fake_stack_words =
        (volatile uintptr_t *)(void *)fake_stack;
    struct interrupt_frame *fake_frame =
        (struct interrupt_frame *)(void *)fake_stack;
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    struct scheduler_task_handle handle;
    enum scheduler_task_state state;
    uint64_t previous_slot_end = 0U;
    uint64_t lower;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper;

    if (!quantum_ticks_are_valid(0U) ||
        !quantum_ticks_are_valid(SCHEDULER_QUANTUM_TICKS - 1U) ||
        quantum_ticks_are_valid(SCHEDULER_QUANTUM_TICKS) ||
        quantum_ticks_are_valid(UINT32_MAX)) {
        return false;
    }

    for (size_t index = 0U; index < 24U; ++index) {
        fake_stack_words[index] = 0U;
    }

    fake_frame->vector = INTERRUPT_RESCHEDULE_VECTOR;
    fake_frame->rip =
        (uintptr_t)(const void *)scheduler_task_first_entry_address;
    fake_frame->cs = CPU_GDT_CODE_SELECTOR;
    fake_frame->rflags = SCHEDULER_RFLAGS_RESERVED | SCHEDULER_RFLAGS_IF;
    fake_frame->rsp = (uintptr_t)(void *)&fake_stack[24];
    fake_frame->ss = CPU_GDT_DATA_SELECTOR;

    if (!saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->ss = 0U;

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->ss = CPU_GDT_DATA_SELECTOR;
    fake_frame->rsp = (uintptr_t)(void *)&fake_stack[24] + sizeof(uint64_t);

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->rsp = (uintptr_t)(void *)&fake_stack[24];

    fake_frame->cs = CPU_GDT_DATA_SELECTOR;

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->cs = CPU_GDT_CODE_SELECTOR;
    fake_frame->rflags = SCHEDULER_RFLAGS_ALLOWED;

    if (!saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->rflags = SCHEDULER_RFLAGS_RESERVED | SCHEDULER_RFLAGS_TF;

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->rflags = SCHEDULER_RFLAGS_RESERVED | SCHEDULER_RFLAGS_RF;

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    if (!prepare_interrupt_rflags_for_resume(&fake_frame->rflags) ||
        fake_frame->rflags != SCHEDULER_RFLAGS_RESERVED ||
        !saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
            true
        )) {
        return false;
    }

    fake_frame->rflags = SCHEDULER_RFLAGS_RESERVED |
        SCHEDULER_RFLAGS_TF | SCHEDULER_RFLAGS_RF;

    if (prepare_interrupt_rflags_for_resume(&fake_frame->rflags) ||
        fake_frame->rflags != (SCHEDULER_RFLAGS_RESERVED |
            SCHEDULER_RFLAGS_TF | SCHEDULER_RFLAGS_RF)) {
        return false;
    }

    fake_frame->rflags = SCHEDULER_RFLAGS_RESERVED | (UINT64_C(1) << 63U);

    if (saved_frame_is_valid(
            fake_frame,
            (uintptr_t)(void *)&fake_stack[0],
            (uintptr_t)(void *)&fake_stack[24],
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

bool scheduler_test_interrupt_while_locked(void)
{
    struct spinlock_irq_token lock_token;

    if (forbidden_context() || !scheduler_initialized || scheduler_poisoned ||
        !scheduler_lock_enter(&lock_token)) {
        return false;
    }

    __asm__ volatile ("int $0x82" : : : "memory");
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        return false;
    }
    return true;
}

static enum scheduler_status scheduler_initialize_unguarded(void)
{
    struct frame_allocator_task_stack_certificate observed_frame;
    struct virtual_memory_task_stack_certificate observed_vm;
    const uintptr_t stack_pointer = cpu_read_stack_pointer();
    enum scheduler_core_status core_status;
    enum xstate_status xstate_status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (scheduler_initialized) {
        return SCHEDULER_STATUS_ALREADY_INITIALIZED;
    }

    resource_certificate_clear();

    xstate_status = xstate_runtime_initialize();
    if (xstate_status != XSTATE_STATUS_OK &&
        xstate_status != XSTATE_STATUS_ALREADY_INITIALIZED) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    if ((uintptr_t)(void *)bootstrap_stack_bottom >=
            (uintptr_t)(void *)bootstrap_stack_top ||
        stack_pointer < (uintptr_t)(void *)bootstrap_stack_bottom ||
        stack_pointer >= (uintptr_t)(void *)bootstrap_stack_top ||
        heap_validate() != HEAP_STATUS_OK ||
        frame_allocator_task_stack_certificate_get(&observed_frame) !=
            FRAME_STATUS_OK ||
        virtual_memory_task_stack_certificate_get(&observed_vm) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        observed_frame.mutation_epoch == 0U ||
        observed_vm.mutation_epoch == 0U ||
        observed_frame.owned_frames != 0U ||
        observed_vm.mapped_pages != 0U) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    scheduler_core_clear(&active_core);
    scheduler_core_clear(&candidate_core);
    bootstrap_saved_frame = NULL;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        runtime_task_clear(&runtime_tasks[index]);
    }

    clear_transaction(SCHEDULER_STACK_PAGE_COUNT);
    scheduler_poisoned = false;
    pending_switch = SCHEDULER_SWITCH_NONE;
    pending_event.index = UINT32_MAX;
    pending_event.epoch = 0U;
    pending_join_target.index = SCHEDULER_INVALID_INDEX;
    pending_join_target.generation = 0U;
    pending_sleep_value = 0U;
    bootstrap_resume_status = SCHEDULER_STATUS_OK;
    quantum_ticks = 0U;
    timer_reschedule_requests = 0U;
    preemption_count = 0U;
    core_status = scheduler_core_initialize(&active_core);

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    scheduler_initialized = true;

    if (validate_state(&active_core) != SCHEDULER_STATUS_OK ||
        !resource_certificate_capture()) {
        scheduler_initialized = false;
        scheduler_core_clear(&active_core);
        resource_certificate_clear();
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
    return scheduler_task_create_with_priority(
        entry,
        opaque_context,
        SCHEDULER_PRIORITY_DEFAULT,
        handle
    );
}

static enum scheduler_status scheduler_task_create_with_priority_unguarded(
    scheduler_task_entry_t entry,
    void *opaque_context,
    uint8_t priority,
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
    core_status = scheduler_core_begin_create_with_priority(
        &candidate_core,
        priority,
        &candidate_handle
    );

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    resource_certificate_clear();
    status = construct_task_stack(
        candidate_handle,
        entry,
        opaque_context
    );

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    if (xstate_runtime_task_create(
            &runtime_tasks[candidate_handle.index].xstate_handle
        ) != XSTATE_STATUS_OK) {
        status = rollback_creation(
            candidate_handle,
            SCHEDULER_STACK_PAGE_COUNT
        );
        return status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_CONTEXT_FAILURE
            : status;
    }
    runtime_tasks[candidate_handle.index].xstate_populated = true;

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

    if (!resource_certificate_capture()) {
        enum scheduler_status rollback_status = rollback_creation(
            candidate_handle,
            SCHEDULER_STACK_PAGE_COUNT
        );

        return rollback_status == SCHEDULER_STATUS_OK
            ? SCHEDULER_STATUS_VALIDATION_FAILURE
            : rollback_status;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    clear_transaction(SCHEDULER_STACK_PAGE_COUNT);
    *handle = candidate_handle;
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_task_set_priority_unguarded(
    struct scheduler_task_handle handle,
    uint8_t priority
)
{
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_local_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_set_priority(
        &candidate_core,
        handle,
        priority
    );

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    status = validate_local_state(&candidate_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_task_get_priority_unguarded(
    struct scheduler_task_handle handle,
    uint8_t *priority
)
{
    enum scheduler_task_state state;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (priority == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    *priority = SCHEDULER_PRIORITY_DEFAULT;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_local_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    core_status = scheduler_core_resolve_handle(&active_core, handle, &state);

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    switch (state) {
    case SCHEDULER_TASK_CONSTRUCTING:
    case SCHEDULER_TASK_READY:
    case SCHEDULER_TASK_RUNNING:
    case SCHEDULER_TASK_BLOCKED:
    case SCHEDULER_TASK_JOIN_BLOCKED:
    case SCHEDULER_TASK_SLEEPING:
    case SCHEDULER_TASK_EXITED:
        *priority = active_core.descriptors[handle.index].priority;
        return SCHEDULER_STATUS_OK;
    default:
        return SCHEDULER_STATUS_INVALID_STATE;
    }
}

static enum scheduler_status scheduler_request_switch(
    enum scheduler_switch_request request,
    struct scheduler_core_event_token event,
    struct scheduler_task_handle target,
    uint64_t sleep_value
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct scheduler_core_identity requester;
    struct spinlock_irq_token lock_token;
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

    /* Reject while an outer preemption guard or any lock pins this CPU. */
    if (!scheduler_switch_is_allowed()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    cpu_interrupt_disable();
    if (!scheduler_lock_enter(&lock_token)) {
        poison_scheduler();
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return SCHEDULER_STATUS_POISONED;
    }

    status = validate_local_state(&active_core);
    if (status == SCHEDULER_STATUS_OK &&
        pending_switch != SCHEDULER_SWITCH_NONE) {
        status = SCHEDULER_STATUS_INVALID_STATE;
    }

    if (status == SCHEDULER_STATUS_OK) {
        requester = active_core.current;
        set_current_resume_status(SCHEDULER_STATUS_INVALID_STATE);
        pending_event = event;
        pending_join_target = target;
        pending_sleep_value = sleep_value;
        pending_switch = request;
    }

    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status;
    }

    /* The scheduler lock is released and IF remains clear across the trap. */
    interrupt_trigger_reschedule();

    if (!scheduler_lock_enter(&lock_token)) {
        poison_scheduler();
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return SCHEDULER_STATUS_POISONED;
    }

    if (!identities_equal(&requester, &active_core.current)) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    } else {
        status = requester.bootstrap
            ? bootstrap_resume_status
            : runtime_tasks[requester.index].resume_status;
    }

    if (status == SCHEDULER_STATUS_OK &&
        request == SCHEDULER_SWITCH_JOIN) {
        struct scheduler_task_handle completion;
        enum scheduler_core_status core_status;

        if (!requester.bootstrap &&
            active_core.descriptors[requester.index].cancel_requested &&
            !active_core.descriptors[requester.index].join_completion_ready) {
            /* Cancellation interrupted the wait; the explicit point exits. */
            goto join_resume_complete;
        }

        scheduler_core_copy(&candidate_core, &active_core);
        core_status = scheduler_core_consume_join_completion(
            &candidate_core,
            &completion
        );
        if (core_status != SCHEDULER_CORE_OK ||
            completion.index != target.index ||
            completion.generation != target.generation ||
            validate_local_state(&candidate_core) != SCHEDULER_STATUS_OK) {
            poison_scheduler();
            status = SCHEDULER_STATUS_POISONED;
        } else {
            scheduler_core_copy(&active_core, &candidate_core);
        }
    }

join_resume_complete:

    /*
     * This public transaction performed the exhaustive audit before
     * publication. Every peer that ran while it was suspended did the same
     * before its own switch, and the resumed tail mutates no allocator, VM,
     * heap, or xstate metadata. Recheck the scheduler-owned state here without
     * repeating those already-completed global audits.
     */
    if (status == SCHEDULER_STATUS_OK &&
        validate_local_state(&active_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    }

    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        status = SCHEDULER_STATUS_POISONED;
    }
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

enum scheduler_status scheduler_yield(void)
{
    return scheduler_request_switch(
        SCHEDULER_SWITCH_YIELD,
        (struct scheduler_core_event_token){UINT32_MAX, 0U},
        (struct scheduler_task_handle){SCHEDULER_INVALID_INDEX, 0U},
        0U
    );
}

static enum scheduler_status scheduler_event_observe_unguarded(
    uint32_t event_index,
    struct scheduler_event_token *token
)
{
    struct scheduler_core_event_token core_token;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (token == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    token->index = SCHEDULER_INVALID_EVENT;
    token->epoch = 0U;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_local_state(&active_core);

    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    core_status = scheduler_core_event_observe(
        &active_core,
        event_index,
        &core_token
    );

    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }

    token->index = core_token.index;
    token->epoch = core_token.epoch;
    return SCHEDULER_STATUS_OK;
}

enum scheduler_status scheduler_event_wait(struct scheduler_event_token token)
{
    const struct scheduler_core_event_token core_token = {
        .index = token.index,
        .epoch = token.epoch
    };
    return scheduler_request_switch(
        SCHEDULER_SWITCH_BLOCK,
        core_token,
        (struct scheduler_task_handle){SCHEDULER_INVALID_INDEX, 0U},
        0U
    );
}

static enum scheduler_status scheduler_event_wake(
    uint32_t event_index,
    bool wake_all,
    size_t *woken_count
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    uint64_t new_epoch;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (woken_count == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }

    *woken_count = 0U;

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
    status = validate_local_state(&active_core);

    if (status == SCHEDULER_STATUS_OK) {
        scheduler_core_copy(&candidate_core, &active_core);
        core_status = scheduler_core_event_signal(
            &candidate_core,
            event_index,
            wake_all,
            woken_count,
            &new_epoch
        );

        if (core_status != SCHEDULER_CORE_OK) {
            status = status_from_core(core_status);
        } else {
            status = validate_local_state(&candidate_core);

            if (status == SCHEDULER_STATUS_OK) {
                scheduler_core_copy(&active_core, &candidate_core);
            }
        }
    }

    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }

    if (status != SCHEDULER_STATUS_OK) {
        *woken_count = 0U;
    }

    return status;
}

static enum scheduler_status scheduler_event_wake_one_unguarded(
    uint32_t event_index,
    size_t *woken_count
)
{
    return scheduler_event_wake(event_index, false, woken_count);
}

static enum scheduler_status scheduler_event_wake_all_unguarded(
    uint32_t event_index,
    size_t *woken_count
)
{
    return scheduler_event_wake(event_index, true, woken_count);
}

enum scheduler_status scheduler_task_join(
    struct scheduler_task_handle handle
)
{
    return scheduler_request_switch(
        SCHEDULER_SWITCH_JOIN,
        (struct scheduler_core_event_token){UINT32_MAX, 0U},
        handle,
        0U
    );
}

enum scheduler_status scheduler_cancellation_point(void)
{
    return scheduler_request_switch(
        SCHEDULER_SWITCH_CANCEL,
        (struct scheduler_core_event_token){UINT32_MAX, 0U},
        (struct scheduler_task_handle){SCHEDULER_INVALID_INDEX, 0U},
        0U
    );
}

enum scheduler_status scheduler_sleep_until(uint64_t deadline)
{
    return scheduler_request_switch(
        SCHEDULER_SWITCH_SLEEP_UNTIL,
        (struct scheduler_core_event_token){UINT32_MAX, 0U},
        (struct scheduler_task_handle){SCHEDULER_INVALID_INDEX, 0U},
        deadline
    );
}

enum scheduler_status scheduler_sleep_for(uint64_t duration)
{
    return scheduler_request_switch(
        SCHEDULER_SWITCH_SLEEP_FOR,
        (struct scheduler_core_event_token){UINT32_MAX, 0U},
        (struct scheduler_task_handle){SCHEDULER_INVALID_INDEX, 0U},
        duration
    );
}

static enum scheduler_status scheduler_task_request_cancel_unguarded(
    struct scheduler_task_handle handle
)
{
    bool made_runnable = false;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    status = validate_local_state(&active_core);
    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }

    scheduler_core_copy(&candidate_core, &active_core);
    core_status = scheduler_core_request_cancel(
        &candidate_core,
        handle,
        &made_runnable
    );
    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }
    status = validate_local_state(&candidate_core);
    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }
    scheduler_core_copy(&active_core, &candidate_core);
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_monotonic_ticks_unguarded(
    uint64_t *ticks
)
{
    enum scheduler_status status;

    if (ticks == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }
    *ticks = 0U;
    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    status = validate_local_state(&active_core);
    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }
    *ticks = active_core.monotonic_tick;
    return SCHEDULER_STATUS_OK;
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
    struct spinlock_irq_token lock_token;

    if (forbidden_context() || !scheduler_initialized || scheduler_poisoned) {
        poison_scheduler();
        console_panic("scheduler task exit used in a forbidden state");
    }

    if (!scheduler_switch_is_allowed()) {
        poison_scheduler();
        console_panic("scheduler task exit attempted while preemption pinned");
    }

    cpu_interrupt_disable();

    if (!scheduler_lock_enter(&lock_token)) {
        poison_scheduler();
        console_panic("scheduler task exit lock acquisition failed");
    }

    if (validate_local_state(&active_core) != SCHEDULER_STATUS_OK) {
        poison_scheduler();
        console_panic("scheduler task exit validation failed");
    }

    if (active_core.current.bootstrap ||
        pending_switch != SCHEDULER_SWITCH_NONE) {
        poison_scheduler();
        console_panic("scheduler task exit could not select a safe peer");
    }

    pending_switch = SCHEDULER_SWITCH_EXIT;
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        console_panic("scheduler task exit lock release failed");
    }
    interrupt_trigger_reschedule();
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

static enum scheduler_status scheduler_task_reap_unguarded(
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

    resource_certificate_clear();

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
        if (!resource_certificate_capture()) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }

        if (interrupts_were_enabled != cpu_interrupts_enabled()) {
            poison_scheduler();
            return SCHEDULER_STATUS_ROLLBACK_FAILURE;
        }

        return SCHEDULER_STATUS_MAPPING_FAILURE;
    }

    if (!saved_task.xstate_populated ||
        xstate_runtime_task_destroy(saved_task.xstate_handle) !=
            XSTATE_STATUS_OK) {
        (void)restore_reap_mappings(handle.index, &saved_task, unmapped);
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
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

    if (!resource_certificate_capture()) {
        poison_scheduler();
        return SCHEDULER_STATUS_ROLLBACK_FAILURE;
    }

    scheduler_core_copy(&active_core, &candidate_core);
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_current_task_unguarded(
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

    status = validate_local_state(&active_core);

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

static enum scheduler_status scheduler_task_query_unguarded(
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
    info->wait_event = SCHEDULER_INVALID_EVENT;
    info->join_target.index = SCHEDULER_INVALID_INDEX;
    info->join_completion.index = SCHEDULER_INVALID_INDEX;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    status = validate_local_state(&active_core);

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
    if (runtime_tasks[handle.index].saved_frame != NULL &&
        saved_frame_is_valid(
            runtime_tasks[handle.index].saved_frame,
            (uintptr_t)runtime_tasks[handle.index].stack_start,
            (uintptr_t)runtime_tasks[handle.index].stack_end,
            runtime_tasks[handle.index].saved_frame_synthetic
        )) {
        info->saved_stack_pointer =
            (uintptr_t)runtime_tasks[handle.index].saved_frame->rsp;
    }
    info->queued = active_core.descriptors[handle.index].queued;
    info->priority = active_core.descriptors[handle.index].priority;
    info->cancellation_requested =
        active_core.descriptors[handle.index].cancel_requested;
    info->join_completion_ready =
        active_core.descriptors[handle.index].join_completion_ready;
    info->stack_canary_intact =
        stack_canary_is_intact(&runtime_tasks[handle.index]);
    info->stack_diagnostics_available =
        state != SCHEDULER_TASK_RUNNING;

    if (info->stack_diagnostics_available &&
        !stack_high_water_measure(
            &runtime_tasks[handle.index],
            &info->stack_high_water_bytes
        )) {
        return SCHEDULER_STATUS_CONTEXT_FAILURE;
    }

    if (state == SCHEDULER_TASK_BLOCKED) {
        info->wait_event =
            (uint32_t)active_core.descriptors[handle.index].wait_event;
        info->wait_epoch = active_core.descriptors[handle.index].wait_epoch;
    }
    if (state == SCHEDULER_TASK_JOIN_BLOCKED) {
        info->join_target.index =
            active_core.descriptors[handle.index].join_index;
        info->join_target.generation =
            active_core.descriptors[handle.index].join_generation;
    }
    if (state == SCHEDULER_TASK_SLEEPING) {
        info->wake_deadline =
            active_core.descriptors[handle.index].wake_deadline;
    }
    if (info->join_completion_ready) {
        info->join_completion.index =
            active_core.descriptors[handle.index].join_completion_index;
        info->join_completion.generation =
            active_core.descriptors[handle.index].join_completion_generation;
    }

    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_task_stack_high_water_unguarded(
    struct scheduler_task_handle handle,
    size_t *high_water_bytes
)
{
    enum scheduler_task_state state;
    enum scheduler_core_status core_status;
    enum scheduler_status status;

    if (high_water_bytes == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }
    *high_water_bytes = 0U;
    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    status = validate_local_state(&active_core);
    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }
    core_status = scheduler_core_resolve_handle(&active_core, handle, &state);
    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }
    if (state == SCHEDULER_TASK_RUNNING) {
        return SCHEDULER_STATUS_RUNNING_TASK;
    }
    return stack_high_water_measure(
        &runtime_tasks[handle.index],
        high_water_bytes
    ) ? SCHEDULER_STATUS_OK : SCHEDULER_STATUS_CONTEXT_FAILURE;
}

static void scheduler_trace_reset(struct scheduler_stack_trace *trace)
{
    if (trace != NULL) {
        bytes_zero(trace, sizeof(*trace));
    }
}

static enum scheduler_status scheduler_task_stack_unwind_unguarded(
    struct scheduler_task_handle handle,
    struct scheduler_stack_trace *trace
)
{
    const struct scheduler_runtime_task *task;
    enum scheduler_task_state state;
    enum scheduler_core_status core_status;
    enum scheduler_status status;
    uintptr_t frame_pointer;

    if (trace == NULL) {
        return SCHEDULER_STATUS_NULL_ARGUMENT;
    }
    scheduler_trace_reset(trace);
    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    status = validate_local_state(&active_core);
    if (status != SCHEDULER_STATUS_OK) {
        return status;
    }
    core_status = scheduler_core_resolve_handle(&active_core, handle, &state);
    if (core_status != SCHEDULER_CORE_OK) {
        return status_from_core(core_status);
    }
    if (state == SCHEDULER_TASK_RUNNING) {
        return SCHEDULER_STATUS_RUNNING_TASK;
    }

    task = &runtime_tasks[handle.index];
    if (!stack_canary_is_intact(task)) {
        return SCHEDULER_STATUS_CONTEXT_FAILURE;
    }
    if (task->saved_frame == NULL) {
        return state == SCHEDULER_TASK_EXITED
            ? SCHEDULER_STATUS_OK
            : SCHEDULER_STATUS_CONTEXT_FAILURE;
    }
    frame_pointer = (uintptr_t)task->saved_frame->rbp;

    while (frame_pointer != 0U) {
        const volatile uintptr_t *record;
        uintptr_t previous_frame;
        uintptr_t return_address;

        if ((frame_pointer & (sizeof(uintptr_t) - 1U)) != 0U ||
            frame_pointer <
                (uintptr_t)task->stack_start + SCHEDULER_STACK_CANARY_SIZE ||
            frame_pointer >
                (uintptr_t)task->stack_end - 2U * sizeof(uintptr_t)) {
            scheduler_trace_reset(trace);
            return SCHEDULER_STATUS_CONTEXT_FAILURE;
        }
        record = (const volatile uintptr_t *)frame_pointer;
        previous_frame = record[0];
        return_address = record[1];
        if (!kernel_text_address(return_address)) {
            scheduler_trace_reset(trace);
            return SCHEDULER_STATUS_CONTEXT_FAILURE;
        }

        if (trace->frame_count == SCHEDULER_STACK_UNWIND_LIMIT) {
            trace->truncated = true;
            return SCHEDULER_STATUS_OK;
        }
        trace->return_addresses[trace->frame_count] = return_address;
        ++trace->frame_count;

        if (previous_frame == 0U) {
            return SCHEDULER_STATUS_OK;
        }
        if (previous_frame <= frame_pointer) {
            scheduler_trace_reset(trace);
            return SCHEDULER_STATUS_CONTEXT_FAILURE;
        }
        frame_pointer = previous_frame;
    }
    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_validate_unguarded(void)
{
    struct preempt_cpu_snapshot snapshot;
    enum scheduler_status status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }

    if (preempt_runtime_snapshot(&snapshot) != PREEMPT_STATUS_OK ||
        !snapshot.online || snapshot.irq_depth != 0U) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    status = validate_state(&active_core);
    if (status == SCHEDULER_STATUS_OK && !resource_certificate_capture()) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }
    return status;
}

static enum scheduler_status scheduler_get_stats_unguarded(
    struct scheduler_stats *stats
)
{
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
    if (!resource_certificate_capture()) {
        return SCHEDULER_STATUS_VALIDATION_FAILURE;
    }

    stats->dynamic_task_limit = SCHEDULER_TASK_LIMIT;
    stats->ready_queue_entries = active_core.queue_count;
    stats->mapped_stack_pages = vm_certificate.mapped_pages;
    stats->task_stack_owned_frames = frame_certificate.owned_frames;
    stats->successful_creations = active_core.successful_creations;
    stats->failed_creations = active_core.failed_creations;
    stats->context_switches = active_core.context_switches;
    stats->completed_tasks = active_core.completed_tasks;
    stats->reaped_tasks = active_core.reaped_tasks;
    stats->blocked_transitions = active_core.blocked_transitions;
    stats->wakeups = active_core.wakeups;
    stats->join_blocks = active_core.join_blocks;
    stats->timed_sleeps = active_core.timed_sleeps;
    stats->cancellation_requests = active_core.cancellation_requests;
    stats->cancellations = active_core.cancellations;
    stats->monotonic_ticks = active_core.monotonic_tick;
    stats->timer_reschedule_requests = timer_reschedule_requests;
    stats->preemptions = preemption_count;
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
        case SCHEDULER_TASK_BLOCKED:
            ++stats->blocked_tasks;
            break;
        case SCHEDULER_TASK_JOIN_BLOCKED:
            ++stats->join_blocked_tasks;
            break;
        case SCHEDULER_TASK_SLEEPING:
            ++stats->sleeping_tasks;
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
            stats->retired_tasks + stats->poisoned_tasks +
            stats->blocked_tasks + stats->join_blocked_tasks +
            stats->sleeping_tasks !=
            SCHEDULER_TASK_LIMIT ||
        stats->ready_tasks +
            (active_core.bootstrap_state == SCHEDULER_TASK_READY ? 1U : 0U) !=
                stats->ready_queue_entries) {
        return SCHEDULER_STATUS_STATS_INVALID;
    }

    return SCHEDULER_STATUS_OK;
}

static enum scheduler_status scheduler_guard_finish(
    struct spinlock_irq_token *token,
    enum scheduler_status status
)
{
    if (!scheduler_lock_leave(token)) {
        poison_scheduler();
        return SCHEDULER_STATUS_POISONED;
    }
    return status;
}

static void scheduler_handle_reset(struct scheduler_task_handle *handle)
{
    if (handle != NULL) {
        handle->index = SCHEDULER_INVALID_INDEX;
        handle->generation = 0U;
    }
}

static void scheduler_priority_reset(uint8_t *priority)
{
    if (priority != NULL) {
        *priority = SCHEDULER_PRIORITY_DEFAULT;
    }
}

static void scheduler_token_reset(struct scheduler_event_token *token)
{
    if (token != NULL) {
        token->index = SCHEDULER_INVALID_EVENT;
        token->epoch = 0U;
    }
}

static void scheduler_identity_reset(struct scheduler_task_identity *identity)
{
    if (identity != NULL) {
        identity->bootstrap = false;
        scheduler_handle_reset(&identity->handle);
    }
}

static void scheduler_info_reset(struct scheduler_task_info *info)
{
    if (info != NULL) {
        bytes_zero(info, sizeof(*info));
        info->wait_event = SCHEDULER_INVALID_EVENT;
        info->join_target.index = SCHEDULER_INVALID_INDEX;
        info->join_completion.index = SCHEDULER_INVALID_INDEX;
    }
}

static void scheduler_stats_reset(struct scheduler_stats *stats)
{
    if (stats != NULL) {
        bytes_zero(stats, sizeof(*stats));
    }
}

enum scheduler_status scheduler_initialize(void)
{
    struct spinlock_irq_token lock_token;
    enum spinlock_status lock_status;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (scheduler_lock.registered == 0U) {
        lock_status = spinlock_runtime_register(
            &scheduler_lock,
            SCHEDULER_LOCK_INDEX,
            SPINLOCK_CLASS_SCHEDULER,
            UINT8_C(0)
        );
        if (lock_status != SPINLOCK_STATUS_OK) {
            return SCHEDULER_STATUS_POISONED;
        }
    }
    if (!scheduler_lock_enter(&lock_token)) {
        return SCHEDULER_STATUS_POISONED;
    }
    return scheduler_guard_finish(
        &lock_token,
        scheduler_initialize_unguarded()
    );
}

enum scheduler_status scheduler_task_create_with_priority(
    scheduler_task_entry_t entry,
    void *opaque_context,
    uint8_t priority,
    struct scheduler_task_handle *handle
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_handle_reset(handle);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_handle_reset(handle);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_handle_reset(handle);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_task_create_with_priority_unguarded(
        entry,
        opaque_context,
        priority,
        handle
    );
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_handle_reset(handle);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_handle_reset(handle);
    }
    return status;
}

enum scheduler_status scheduler_task_set_priority(
    struct scheduler_task_handle handle,
    uint8_t priority
)
{
    struct spinlock_irq_token lock_token;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        return SCHEDULER_STATUS_POISONED;
    }
    return scheduler_guard_finish(
        &lock_token,
        scheduler_task_set_priority_unguarded(handle, priority)
    );
}

enum scheduler_status scheduler_task_get_priority(
    struct scheduler_task_handle handle,
    uint8_t *priority
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_priority_reset(priority);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_priority_reset(priority);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_priority_reset(priority);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_task_get_priority_unguarded(handle, priority);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_priority_reset(priority);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_priority_reset(priority);
    }
    return status;
}

enum scheduler_status scheduler_event_observe(
    uint32_t event_index,
    struct scheduler_event_token *token
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_token_reset(token);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_token_reset(token);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_token_reset(token);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_event_observe_unguarded(event_index, token);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_token_reset(token);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_token_reset(token);
    }
    return status;
}

enum scheduler_status scheduler_event_wake_one(
    uint32_t event_index,
    size_t *woken_count
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_event_wake_one_unguarded(event_index, woken_count);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK && woken_count != NULL) {
        *woken_count = 0U;
    }
    return status;
}

enum scheduler_status scheduler_event_wake_all(
    uint32_t event_index,
    size_t *woken_count
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_event_wake_all_unguarded(event_index, woken_count);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        if (woken_count != NULL) {
            *woken_count = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK && woken_count != NULL) {
        *woken_count = 0U;
    }
    return status;
}

enum scheduler_status scheduler_task_request_cancel(
    struct scheduler_task_handle handle
)
{
    struct spinlock_irq_token lock_token;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        return SCHEDULER_STATUS_POISONED;
    }
    return scheduler_guard_finish(
        &lock_token,
        scheduler_task_request_cancel_unguarded(handle)
    );
}

enum scheduler_status scheduler_monotonic_ticks(uint64_t *ticks)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        if (ticks != NULL) {
            *ticks = 0U;
        }
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        if (ticks != NULL) {
            *ticks = 0U;
        }
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        if (ticks != NULL) {
            *ticks = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_monotonic_ticks_unguarded(ticks);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        if (ticks != NULL) {
            *ticks = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK && ticks != NULL) {
        *ticks = 0U;
    }
    return status;
}

enum scheduler_status scheduler_task_stack_high_water(
    struct scheduler_task_handle handle,
    size_t *high_water_bytes
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        if (high_water_bytes != NULL) {
            *high_water_bytes = 0U;
        }
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        if (high_water_bytes != NULL) {
            *high_water_bytes = 0U;
        }
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        if (high_water_bytes != NULL) {
            *high_water_bytes = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_task_stack_high_water_unguarded(
        handle,
        high_water_bytes
    );
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        if (high_water_bytes != NULL) {
            *high_water_bytes = 0U;
        }
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK && high_water_bytes != NULL) {
        *high_water_bytes = 0U;
    }
    return status;
}

enum scheduler_status scheduler_task_stack_unwind(
    struct scheduler_task_handle handle,
    struct scheduler_stack_trace *trace
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_trace_reset(trace);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_trace_reset(trace);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_trace_reset(trace);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_task_stack_unwind_unguarded(handle, trace);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_trace_reset(trace);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_trace_reset(trace);
    }
    return status;
}

enum scheduler_status scheduler_task_reap(
    struct scheduler_task_handle handle
)
{
    struct spinlock_irq_token lock_token;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        return SCHEDULER_STATUS_POISONED;
    }
    return scheduler_guard_finish(
        &lock_token,
        scheduler_task_reap_unguarded(handle)
    );
}

enum scheduler_status scheduler_current_task(
    struct scheduler_task_identity *identity
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_identity_reset(identity);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_identity_reset(identity);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_identity_reset(identity);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_current_task_unguarded(identity);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_identity_reset(identity);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_identity_reset(identity);
    }
    return status;
}

enum scheduler_status scheduler_task_query(
    struct scheduler_task_handle handle,
    struct scheduler_task_info *info
)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_info_reset(info);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_info_reset(info);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_info_reset(info);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_task_query_unguarded(handle, info);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_info_reset(info);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_info_reset(info);
    }
    return status;
}

enum scheduler_status scheduler_validate(void)
{
    struct spinlock_irq_token lock_token;

    if (forbidden_context()) {
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        return SCHEDULER_STATUS_POISONED;
    }
    return scheduler_guard_finish(
        &lock_token,
        scheduler_validate_unguarded()
    );
}

enum scheduler_status scheduler_get_stats(struct scheduler_stats *stats)
{
    struct spinlock_irq_token lock_token;
    enum scheduler_status status;

    if (forbidden_context()) {
        scheduler_stats_reset(stats);
        return SCHEDULER_STATUS_FORBIDDEN_CONTEXT;
    }
    if (!scheduler_initialized) {
        scheduler_stats_reset(stats);
        return SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (!scheduler_lock_enter(&lock_token)) {
        scheduler_stats_reset(stats);
        return SCHEDULER_STATUS_POISONED;
    }
    status = scheduler_get_stats_unguarded(stats);
    if (!scheduler_lock_leave(&lock_token)) {
        poison_scheduler();
        scheduler_stats_reset(stats);
        return SCHEDULER_STATUS_POISONED;
    }
    if (status != SCHEDULER_STATUS_OK) {
        scheduler_stats_reset(stats);
    }
    return status;
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
        return "scheduler switch/use is forbidden in this pinned context";
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
    case SCHEDULER_STATUS_INVALID_PRIORITY:
        return "scheduler priority is outside the fixed range";
    case SCHEDULER_STATUS_INVALID_EVENT:
        return "scheduler event index is outside the fixed range";
    case SCHEDULER_STATUS_EVENT_CHANGED:
        return "scheduler event changed before the task could block";
    case SCHEDULER_STATUS_EVENT_EPOCH_EXHAUSTED:
        return "scheduler event epoch is permanently exhausted";
    case SCHEDULER_STATUS_BLOCKED_TASK:
        return "a blocked task cannot be reaped";
    case SCHEDULER_STATUS_SELF_JOIN:
        return "a task cannot join itself";
    case SCHEDULER_STATUS_JOIN_CYCLE:
        return "join dependency would create a cycle";
    case SCHEDULER_STATUS_JOIN_COMPLETION_PENDING:
        return "a retained join completion must be consumed first";
    case SCHEDULER_STATUS_CANCELLATION_NOT_REQUESTED:
        return "the current task has no pending cancellation request";
    case SCHEDULER_STATUS_INVALID_DEADLINE:
        return "sleep deadline must be later than the current tick";
    case SCHEDULER_STATUS_TIME_REGRESSION:
        return "scheduler monotonic time cannot move backwards";
    case SCHEDULER_STATUS_TICK_EXHAUSTED:
        return "scheduler monotonic tick domain is exhausted";
    case SCHEDULER_STATUS_DEADLINE_OVERFLOW:
        return "relative sleep deadline overflowed";
    default:
        return "unknown scheduler status";
    }
}

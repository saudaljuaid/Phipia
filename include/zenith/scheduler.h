/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_H
#define ZENITH_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEDULER_TASK_LIMIT ((size_t)16U)
#define SCHEDULER_INVALID_INDEX UINT32_MAX
#define SCHEDULER_PRIORITY_MIN UINT8_C(0)
#define SCHEDULER_PRIORITY_MAX UINT8_C(7)
#define SCHEDULER_PRIORITY_DEFAULT SCHEDULER_PRIORITY_MIN
#define SCHEDULER_EVENT_LIMIT ((size_t)8U)
#define SCHEDULER_INVALID_EVENT UINT32_MAX
#define SCHEDULER_QUANTUM_TICKS UINT32_C(4)
#define SCHEDULER_STACK_UNWIND_LIMIT ((size_t)32U)

typedef void (*scheduler_task_entry_t)(void *context);
struct interrupt_frame;

enum scheduler_task_state {
    SCHEDULER_TASK_UNUSED = 0,
    SCHEDULER_TASK_CONSTRUCTING,
    SCHEDULER_TASK_READY,
    SCHEDULER_TASK_RUNNING,
    SCHEDULER_TASK_EXITED,
    SCHEDULER_TASK_REAPING,
    SCHEDULER_TASK_RETIRED,
    SCHEDULER_TASK_POISONED,
    /* Waiting for a scheduler event token. */
    SCHEDULER_TASK_BLOCKED,
    SCHEDULER_TASK_JOIN_BLOCKED,
    SCHEDULER_TASK_SLEEPING
};

enum scheduler_status {
    SCHEDULER_STATUS_OK = 0,
    SCHEDULER_STATUS_NULL_ARGUMENT,
    SCHEDULER_STATUS_ALREADY_INITIALIZED,
    SCHEDULER_STATUS_NOT_INITIALIZED,
    SCHEDULER_STATUS_FORBIDDEN_CONTEXT,
    SCHEDULER_STATUS_POISONED,
    SCHEDULER_STATUS_INVALID_ENTRY,
    SCHEDULER_STATUS_INVALID_HANDLE,
    SCHEDULER_STATUS_STALE_HANDLE,
    SCHEDULER_STATUS_GENERATION_EXHAUSTED,
    SCHEDULER_STATUS_DESCRIPTOR_LIMIT,
    SCHEDULER_STATUS_QUEUE_LIMIT,
    SCHEDULER_STATUS_NO_RUNNABLE_PEER,
    SCHEDULER_STATUS_INVALID_STATE,
    SCHEDULER_STATUS_ARITHMETIC_OVERFLOW,
    SCHEDULER_STATUS_OUT_OF_MEMORY,
    SCHEDULER_STATUS_FRAME_FAILURE,
    SCHEDULER_STATUS_MAPPING_FAILURE,
    SCHEDULER_STATUS_CONTEXT_FAILURE,
    SCHEDULER_STATUS_ROLLBACK_FAILURE,
    SCHEDULER_STATUS_RUNNING_TASK,
    SCHEDULER_STATUS_RUNNABLE_TASK,
    SCHEDULER_STATUS_DOUBLE_REAP,
    SCHEDULER_STATUS_BOOTSTRAP_TASK,
    SCHEDULER_STATUS_STATS_INVALID,
    SCHEDULER_STATUS_VALIDATION_FAILURE,
    SCHEDULER_STATUS_TEST_FAILURE,
    SCHEDULER_STATUS_INVALID_PRIORITY,
    SCHEDULER_STATUS_INVALID_EVENT,
    SCHEDULER_STATUS_EVENT_CHANGED,
    SCHEDULER_STATUS_EVENT_EPOCH_EXHAUSTED,
    SCHEDULER_STATUS_BLOCKED_TASK,
    SCHEDULER_STATUS_SELF_JOIN,
    SCHEDULER_STATUS_JOIN_CYCLE,
    SCHEDULER_STATUS_JOIN_COMPLETION_PENDING,
    SCHEDULER_STATUS_CANCELLATION_NOT_REQUESTED,
    SCHEDULER_STATUS_INVALID_DEADLINE,
    SCHEDULER_STATUS_TIME_REGRESSION,
    SCHEDULER_STATUS_TICK_EXHAUSTED,
    SCHEDULER_STATUS_DEADLINE_OVERFLOW
};

struct scheduler_task_handle {
    uint32_t index;
    uint64_t generation;
};

struct scheduler_task_identity {
    bool bootstrap;
    struct scheduler_task_handle handle;
};

struct scheduler_event_token {
    uint32_t index;
    uint64_t epoch;
};

struct scheduler_task_info {
    enum scheduler_task_state state;
    uint64_t lower_guard;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper_guard;
    /* Resume-time RSP, or zero when the task has no saved continuation. */
    uintptr_t saved_stack_pointer;
    bool queued;
    uint8_t priority;
    uint32_t wait_event;
    uint64_t wait_epoch;
    uint64_t wake_deadline;
    struct scheduler_task_handle join_target;
    struct scheduler_task_handle join_completion;
    bool cancellation_requested;
    bool join_completion_ready;
    size_t stack_high_water_bytes;
    bool stack_canary_intact;
    bool stack_diagnostics_available;
};

struct scheduler_stack_trace {
    uintptr_t return_addresses[SCHEDULER_STACK_UNWIND_LIMIT];
    size_t frame_count;
    bool truncated;
};

struct scheduler_stats {
    size_t dynamic_task_limit;
    size_t unused_tasks;
    size_t constructing_tasks;
    size_t ready_tasks;
    size_t running_dynamic_tasks;
    size_t exited_tasks;
    size_t reaping_tasks;
    size_t retired_tasks;
    size_t poisoned_tasks;
    size_t blocked_tasks;
    size_t join_blocked_tasks;
    size_t sleeping_tasks;
    size_t ready_queue_entries;
    size_t mapped_stack_pages;
    size_t task_stack_owned_frames;
    size_t successful_creations;
    size_t failed_creations;
    size_t context_switches;
    size_t completed_tasks;
    size_t reaped_tasks;
    size_t blocked_transitions;
    size_t wakeups;
    size_t join_blocks;
    size_t timed_sleeps;
    size_t cancellation_requests;
    size_t cancellations;
    uint64_t monotonic_ticks;
    size_t timer_reschedule_requests;
    size_t preemptions;
    bool bootstrap_running;
    bool poisoned;
};

bool scheduler_self_test(void);
enum scheduler_status scheduler_initialize(void);
enum scheduler_status scheduler_task_create(
    scheduler_task_entry_t entry,
    void *context,
    struct scheduler_task_handle *handle
);
enum scheduler_status scheduler_task_create_with_priority(
    scheduler_task_entry_t entry,
    void *context,
    uint8_t priority,
    struct scheduler_task_handle *handle
);
enum scheduler_status scheduler_task_set_priority(
    struct scheduler_task_handle handle,
    uint8_t priority
);
enum scheduler_status scheduler_task_get_priority(
    struct scheduler_task_handle handle,
    uint8_t *priority
);
enum scheduler_status scheduler_yield(void);
enum scheduler_status scheduler_event_observe(
    uint32_t event_index,
    struct scheduler_event_token *token
);
enum scheduler_status scheduler_event_wait(
    struct scheduler_event_token token
);
enum scheduler_status scheduler_event_wake_one(
    uint32_t event_index,
    size_t *woken_count
);
enum scheduler_status scheduler_event_wake_all(
    uint32_t event_index,
    size_t *woken_count
);
/* Join completion is retained by the waiter, so target reaping or descriptor
 * reuse cannot turn a completed join into a stale-handle failure. Joining is
 * forbidden for bootstrap, self, and any dependency cycle. */
enum scheduler_status scheduler_task_join(
    struct scheduler_task_handle handle
);
/* Cancellation requests are idempotent and deferred. A blocked target is
 * made ready, but it exits only at scheduler_cancellation_point(). */
enum scheduler_status scheduler_task_request_cancel(
    struct scheduler_task_handle handle
);
enum scheduler_status scheduler_cancellation_point(void);
/* Deadlines occupy a monotonic uint64_t tick domain. Past/zero-duration sleeps
 * are rejected, relative overflow is reported, and UINT64_MAX is the final
 * representable deadline. */
enum scheduler_status scheduler_sleep_until(uint64_t deadline);
enum scheduler_status scheduler_sleep_for(uint64_t duration);
enum scheduler_status scheduler_task_stack_high_water(
    struct scheduler_task_handle handle,
    size_t *high_water_bytes
);
enum scheduler_status scheduler_task_stack_unwind(
    struct scheduler_task_handle handle,
    struct scheduler_stack_trace *trace
);
enum scheduler_status scheduler_monotonic_ticks(uint64_t *ticks);
_Noreturn void scheduler_task_exit(void);
enum scheduler_status scheduler_task_reap(
    struct scheduler_task_handle handle
);
enum scheduler_status scheduler_current_task(
    struct scheduler_task_identity *identity
);
enum scheduler_status scheduler_task_query(
    struct scheduler_task_handle handle,
    struct scheduler_task_info *info
);
enum scheduler_status scheduler_validate(void);
enum scheduler_status scheduler_get_stats(struct scheduler_stats *stats);
void scheduler_timer_tick(void);
struct interrupt_frame *scheduler_interrupt_exit(
    struct interrupt_frame *frame,
    uint8_t vector,
    bool reschedule_eligible
);
bool scheduler_interrupt_return_frame_is_valid(
    const struct interrupt_frame *frame
);
const char *scheduler_status_string(enum scheduler_status status);

#endif

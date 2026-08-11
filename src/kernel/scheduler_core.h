/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_CORE_H
#define ZENITH_SCHEDULER_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/scheduler.h>

#define SCHEDULER_CORE_QUEUE_CAPACITY (SCHEDULER_TASK_LIMIT + (size_t)1U)
#define SCHEDULER_CORE_EVENT_LIMIT ((size_t)8U)
#define SCHEDULER_CORE_EVENT_NONE UINT8_MAX
#define SCHEDULER_CORE_QUEUE_UNUSED UINT8_MAX
#define SCHEDULER_CORE_QUEUE_BOOTSTRAP ((uint8_t)SCHEDULER_TASK_LIMIT)
#define SCHEDULER_CORE_AGING_INTERVAL UINT8_C(8)
#define SCHEDULER_CORE_MAXIMUM_WAIT_AGE \
    ((uint8_t)(SCHEDULER_PRIORITY_MAX * SCHEDULER_CORE_AGING_INTERVAL))

struct scheduler_core_identity {
    bool bootstrap;
    uint32_t index;
    uint64_t generation;
};

struct scheduler_core_descriptor {
    enum scheduler_task_state state;
    uint8_t priority;
    uint8_t wait_age;
    bool queued;
    uint8_t wait_event;
    uint64_t generation;
    uint64_t wait_epoch;
    uint64_t wake_deadline;
    uint64_t join_generation;
    uint64_t join_completion_generation;
    uint32_t join_index;
    uint32_t join_completion_index;
    bool cancel_requested;
    bool join_completion_ready;
    uint8_t reserved[6];
};

struct scheduler_core_event_token {
    uint32_t index;
    uint64_t epoch;
};

struct scheduler_core_state {
    struct scheduler_core_descriptor descriptors[SCHEDULER_TASK_LIMIT];
    uint64_t ready_generations[SCHEDULER_CORE_QUEUE_CAPACITY];
    uint8_t ready_identities[SCHEDULER_CORE_QUEUE_CAPACITY];
    struct scheduler_core_identity current;
    uint64_t event_epochs[SCHEDULER_CORE_EVENT_LIMIT];
    uint16_t event_waiters[SCHEDULER_CORE_EVENT_LIMIT];
    uint16_t join_waiters[SCHEDULER_TASK_LIMIT];
    uint64_t monotonic_tick;
    size_t queue_head;
    size_t queue_count;
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
    enum scheduler_task_state bootstrap_state;
    uint8_t bootstrap_wait_age;
    bool initialized;
    bool poisoned;
    uint8_t reserved[9];
};

enum scheduler_core_status {
    SCHEDULER_CORE_OK = 0,
    SCHEDULER_CORE_NULL_ARGUMENT,
    SCHEDULER_CORE_ALREADY_INITIALIZED,
    SCHEDULER_CORE_NOT_INITIALIZED,
    SCHEDULER_CORE_POISONED,
    SCHEDULER_CORE_INVALID_HANDLE,
    SCHEDULER_CORE_STALE_HANDLE,
    SCHEDULER_CORE_GENERATION_EXHAUSTED,
    SCHEDULER_CORE_DESCRIPTOR_LIMIT,
    SCHEDULER_CORE_QUEUE_LIMIT,
    SCHEDULER_CORE_NO_RUNNABLE_PEER,
    SCHEDULER_CORE_INVALID_STATE,
    SCHEDULER_CORE_RUNNING_TASK,
    SCHEDULER_CORE_RUNNABLE_TASK,
    SCHEDULER_CORE_DOUBLE_REAP,
    SCHEDULER_CORE_BOOTSTRAP_TASK,
    SCHEDULER_CORE_STATS_INVALID,
    SCHEDULER_CORE_CORRUPTED,
    SCHEDULER_CORE_INVALID_PRIORITY,
    SCHEDULER_CORE_INVALID_EVENT,
    SCHEDULER_CORE_EVENT_CHANGED,
    SCHEDULER_CORE_EVENT_EPOCH_EXHAUSTED,
    SCHEDULER_CORE_BLOCKED_TASK,
    SCHEDULER_CORE_SELF_JOIN,
    SCHEDULER_CORE_JOIN_CYCLE,
    SCHEDULER_CORE_JOIN_COMPLETION_PENDING,
    SCHEDULER_CORE_CANCELLATION_NOT_REQUESTED,
    SCHEDULER_CORE_INVALID_DEADLINE,
    SCHEDULER_CORE_TIME_REGRESSION,
    SCHEDULER_CORE_TICK_EXHAUSTED,
    SCHEDULER_CORE_DEADLINE_OVERFLOW
};

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void scheduler_core_clear(struct scheduler_core_state *state);
void scheduler_core_copy(
    struct scheduler_core_state *destination,
    const struct scheduler_core_state *source
);
enum scheduler_core_status scheduler_core_initialize(
    struct scheduler_core_state *state
);
enum scheduler_core_status scheduler_core_validate(
    const struct scheduler_core_state *state
);
enum scheduler_core_status scheduler_core_begin_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle *handle
);
enum scheduler_core_status scheduler_core_begin_create_with_priority(
    struct scheduler_core_state *state,
    uint8_t priority,
    struct scheduler_task_handle *handle
);
enum scheduler_core_status scheduler_core_publish_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_abort_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_set_priority(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    uint8_t priority
);
enum scheduler_core_status scheduler_core_yield(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_exit_current(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_begin_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_finish_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_abort_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_event_observe(
    const struct scheduler_core_state *state,
    uint32_t event_index,
    struct scheduler_core_event_token *token
);
enum scheduler_core_status scheduler_core_block_current(
    struct scheduler_core_state *state,
    struct scheduler_core_event_token token,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_event_signal(
    struct scheduler_core_state *state,
    uint32_t event_index,
    bool wake_all,
    size_t *woken_count,
    uint64_t *new_epoch
);
enum scheduler_core_status scheduler_core_join_current(
    struct scheduler_core_state *state,
    struct scheduler_task_handle target,
    bool *completed,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
/* Consume the current dynamic task's retained completion before it begins a
 * different blocking operation. The cached handle remains valid as a
 * completion record even after the target descriptor is reused. */
enum scheduler_core_status scheduler_core_consume_join_completion(
    struct scheduler_core_state *state,
    struct scheduler_task_handle *completion
);
enum scheduler_core_status scheduler_core_request_cancel(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    bool *made_runnable
);
enum scheduler_core_status scheduler_core_cancellation_point(
    struct scheduler_core_state *state,
    bool *cancelled,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_sleep_current_until(
    struct scheduler_core_state *state,
    uint64_t deadline,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_sleep_current_for(
    struct scheduler_core_state *state,
    uint64_t duration,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
);
enum scheduler_core_status scheduler_core_advance_time(
    struct scheduler_core_state *state,
    uint64_t new_tick,
    size_t *woken_count
);
/* Advances exactly one tick, unless the monotonic domain is exhausted. Both
 * time entry points scan at most SCHEDULER_TASK_LIMIT descriptors. */
enum scheduler_core_status scheduler_core_timer_tick(
    struct scheduler_core_state *state,
    size_t *woken_count
);
enum scheduler_core_status scheduler_core_resolve_handle(
    const struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    enum scheduler_task_state *task_state
);

#endif

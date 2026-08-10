/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_CORE_H
#define ZENITH_SCHEDULER_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/scheduler.h>

#define SCHEDULER_CORE_QUEUE_CAPACITY (SCHEDULER_TASK_LIMIT + (size_t)1U)

struct scheduler_core_identity {
    bool bootstrap;
    uint32_t index;
    uint64_t generation;
};

struct scheduler_core_descriptor {
    enum scheduler_task_state state;
    uint64_t generation;
    bool queued;
};

struct scheduler_core_state {
    struct scheduler_core_descriptor descriptors[SCHEDULER_TASK_LIMIT];
    struct scheduler_core_identity ready_queue[SCHEDULER_CORE_QUEUE_CAPACITY];
    struct scheduler_core_identity current;
    size_t queue_head;
    size_t queue_count;
    size_t successful_creations;
    size_t failed_creations;
    size_t context_switches;
    size_t completed_tasks;
    size_t reaped_tasks;
    enum scheduler_task_state bootstrap_state;
    bool initialized;
    bool poisoned;
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
    SCHEDULER_CORE_CORRUPTED
};

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
enum scheduler_core_status scheduler_core_publish_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
);
enum scheduler_core_status scheduler_core_abort_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
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
enum scheduler_core_status scheduler_core_resolve_handle(
    const struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    enum scheduler_task_state *task_state
);

#endif

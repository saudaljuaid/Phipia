/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_H
#define ZENITH_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEDULER_TASK_LIMIT ((size_t)16U)
#define SCHEDULER_INVALID_INDEX UINT32_MAX

typedef void (*scheduler_task_entry_t)(void *context);

enum scheduler_task_state {
    SCHEDULER_TASK_UNUSED = 0,
    SCHEDULER_TASK_CONSTRUCTING,
    SCHEDULER_TASK_READY,
    SCHEDULER_TASK_RUNNING,
    SCHEDULER_TASK_EXITED,
    SCHEDULER_TASK_REAPING,
    SCHEDULER_TASK_RETIRED,
    SCHEDULER_TASK_POISONED
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
    SCHEDULER_STATUS_TEST_FAILURE
};

struct scheduler_task_handle {
    uint32_t index;
    uint64_t generation;
};

struct scheduler_task_identity {
    bool bootstrap;
    struct scheduler_task_handle handle;
};

struct scheduler_task_info {
    enum scheduler_task_state state;
    uint64_t lower_guard;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t upper_guard;
    uintptr_t saved_stack_pointer;
    bool queued;
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
    size_t ready_queue_entries;
    size_t mapped_stack_pages;
    size_t task_stack_owned_frames;
    size_t successful_creations;
    size_t failed_creations;
    size_t context_switches;
    size_t completed_tasks;
    size_t reaped_tasks;
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
enum scheduler_status scheduler_yield(void);
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
const char *scheduler_status_string(enum scheduler_status status);

#endif

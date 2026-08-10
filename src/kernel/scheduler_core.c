/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scheduler_core.h"

static void identity_clear(struct scheduler_core_identity *identity)
{
    identity->bootstrap = false;
    identity->index = SCHEDULER_INVALID_INDEX;
    identity->generation = 0U;
}

static bool identity_is_clear(const struct scheduler_core_identity *identity)
{
    return !identity->bootstrap &&
        identity->index == SCHEDULER_INVALID_INDEX &&
        identity->generation == 0U;
}

static bool identity_is_valid(
    const struct scheduler_core_state *state,
    const struct scheduler_core_identity *identity,
    enum scheduler_task_state required_state
)
{
    if (identity->bootstrap) {
        return identity->index == SCHEDULER_INVALID_INDEX &&
            identity->generation == 0U &&
            state->bootstrap_state == required_state;
    }

    return identity->index < SCHEDULER_TASK_LIMIT &&
        identity->generation != 0U &&
        state->descriptors[identity->index].generation ==
            identity->generation &&
        state->descriptors[identity->index].state == required_state;
}

static struct scheduler_core_identity bootstrap_identity(void)
{
    struct scheduler_core_identity identity;

    identity.bootstrap = true;
    identity.index = SCHEDULER_INVALID_INDEX;
    identity.generation = 0U;
    return identity;
}

static struct scheduler_core_identity dynamic_identity(
    uint32_t index,
    uint64_t generation
)
{
    struct scheduler_core_identity identity;

    identity.bootstrap = false;
    identity.index = index;
    identity.generation = generation;
    return identity;
}

static enum scheduler_core_status queue_push(
    struct scheduler_core_state *state,
    struct scheduler_core_identity identity
)
{
    size_t tail;

    if (state->queue_count >= SCHEDULER_CORE_QUEUE_CAPACITY) {
        return SCHEDULER_CORE_QUEUE_LIMIT;
    }

    tail = state->queue_head + state->queue_count;

    if (tail >= SCHEDULER_CORE_QUEUE_CAPACITY) {
        tail -= SCHEDULER_CORE_QUEUE_CAPACITY;
    }

    if (!identity_is_clear(&state->ready_queue[tail])) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    state->ready_queue[tail] = identity;
    ++state->queue_count;

    if (identity.bootstrap) {
        state->bootstrap_state = SCHEDULER_TASK_READY;
    } else {
        state->descriptors[identity.index].state = SCHEDULER_TASK_READY;
        state->descriptors[identity.index].queued = true;
    }

    return SCHEDULER_CORE_OK;
}

static enum scheduler_core_status queue_pop(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *identity
)
{
    if (identity == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    *identity = state->ready_queue[state->queue_head];
    identity_clear(&state->ready_queue[state->queue_head]);
    ++state->queue_head;

    if (state->queue_head == SCHEDULER_CORE_QUEUE_CAPACITY) {
        state->queue_head = 0U;
    }

    --state->queue_count;

    if (!identity_is_valid(state, identity, SCHEDULER_TASK_READY)) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    if (identity->bootstrap) {
        state->bootstrap_state = SCHEDULER_TASK_RUNNING;
    } else {
        state->descriptors[identity->index].state = SCHEDULER_TASK_RUNNING;
        state->descriptors[identity->index].queued = false;
    }

    state->current = *identity;
    return SCHEDULER_CORE_OK;
}

void scheduler_core_clear(struct scheduler_core_state *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        state->descriptors[index].state = SCHEDULER_TASK_UNUSED;
        state->descriptors[index].generation = 0U;
        state->descriptors[index].queued = false;
    }

    for (size_t index = 0U;
         index < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++index) {
        identity_clear(&state->ready_queue[index]);
    }

    identity_clear(&state->current);
    state->queue_head = 0U;
    state->queue_count = 0U;
    state->successful_creations = 0U;
    state->failed_creations = 0U;
    state->context_switches = 0U;
    state->completed_tasks = 0U;
    state->reaped_tasks = 0U;
    state->bootstrap_state = SCHEDULER_TASK_UNUSED;
    state->initialized = false;
    state->poisoned = false;
}

void scheduler_core_copy(
    struct scheduler_core_state *destination,
    const struct scheduler_core_state *source
)
{
    if (destination == NULL || source == NULL) {
        return;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        destination->descriptors[index] = source->descriptors[index];
    }

    for (size_t index = 0U;
         index < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++index) {
        destination->ready_queue[index] = source->ready_queue[index];
    }

    destination->current = source->current;
    destination->queue_head = source->queue_head;
    destination->queue_count = source->queue_count;
    destination->successful_creations = source->successful_creations;
    destination->failed_creations = source->failed_creations;
    destination->context_switches = source->context_switches;
    destination->completed_tasks = source->completed_tasks;
    destination->reaped_tasks = source->reaped_tasks;
    destination->bootstrap_state = source->bootstrap_state;
    destination->initialized = source->initialized;
    destination->poisoned = source->poisoned;
}

enum scheduler_core_status scheduler_core_initialize(
    struct scheduler_core_state *state
)
{
    if (state == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    if (state->initialized) {
        return SCHEDULER_CORE_ALREADY_INITIALIZED;
    }

    scheduler_core_clear(state);

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        state->descriptors[index].generation = 1U;
    }

    state->bootstrap_state = SCHEDULER_TASK_RUNNING;
    state->current = bootstrap_identity();
    state->initialized = true;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_validate(
    const struct scheduler_core_state *state
)
{
    bool observed_queue[SCHEDULER_TASK_LIMIT] = {false};
    bool bootstrap_queued = false;
    size_t running_count = 0U;

    if (state == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    if (!state->initialized) {
        return SCHEDULER_CORE_NOT_INITIALIZED;
    }

    if (state->poisoned) {
        return SCHEDULER_CORE_POISONED;
    }

    if (state->queue_head >= SCHEDULER_CORE_QUEUE_CAPACITY ||
        state->queue_count > SCHEDULER_CORE_QUEUE_CAPACITY ||
        !identity_is_valid(
            state,
            &state->current,
            SCHEDULER_TASK_RUNNING
        )) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    if (state->bootstrap_state == SCHEDULER_TASK_RUNNING) {
        ++running_count;
    } else if (state->bootstrap_state != SCHEDULER_TASK_READY) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    for (size_t offset = 0U;
         offset < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++offset) {
        size_t position = state->queue_head + offset;
        bool active = offset < state->queue_count;
        const struct scheduler_core_identity *identity;

        if (position >= SCHEDULER_CORE_QUEUE_CAPACITY) {
            position -= SCHEDULER_CORE_QUEUE_CAPACITY;
        }

        identity = &state->ready_queue[position];

        if (!active) {
            if (!identity_is_clear(identity)) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            continue;
        }

        if (!identity_is_valid(state, identity, SCHEDULER_TASK_READY)) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        if (identity->bootstrap) {
            if (bootstrap_queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            bootstrap_queued = true;
        } else {
            if (observed_queue[identity->index]) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            observed_queue[identity->index] = true;
        }
    }

    if ((state->bootstrap_state == SCHEDULER_TASK_READY) !=
            bootstrap_queued) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const struct scheduler_core_descriptor *descriptor =
            &state->descriptors[index];

        if (descriptor->generation == 0U ||
            descriptor->queued != observed_queue[index]) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        switch (descriptor->state) {
        case SCHEDULER_TASK_UNUSED:
            if (descriptor->queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_CONSTRUCTING:
        case SCHEDULER_TASK_EXITED:
        case SCHEDULER_TASK_REAPING:
            if (descriptor->queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_READY:
            if (!descriptor->queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_RUNNING:
            if (descriptor->queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            ++running_count;
            break;
        case SCHEDULER_TASK_RETIRED:
            if (descriptor->generation != UINT64_MAX || descriptor->queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_POISONED:
        default:
            return SCHEDULER_CORE_CORRUPTED;
        }
    }

    if (running_count != 1U) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_resolve_handle(
    const struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    enum scheduler_task_state *task_state
)
{
    enum scheduler_core_status status;

    if (task_state == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *task_state = SCHEDULER_TASK_UNUSED;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (handle.index >= SCHEDULER_TASK_LIMIT || handle.generation == 0U) {
        return SCHEDULER_CORE_INVALID_HANDLE;
    }

    if (state->descriptors[handle.index].generation != handle.generation) {
        return SCHEDULER_CORE_STALE_HANDLE;
    }

    *task_state = state->descriptors[handle.index].state;
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_begin_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle *handle
)
{
    enum scheduler_core_status status;
    bool retired_seen = false;

    if (handle == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    handle->index = SCHEDULER_INVALID_INDEX;
    handle->generation = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        struct scheduler_core_descriptor *descriptor =
            &state->descriptors[index];

        if (descriptor->state == SCHEDULER_TASK_RETIRED) {
            retired_seen = true;
            continue;
        }

        if (descriptor->state != SCHEDULER_TASK_UNUSED) {
            continue;
        }

        descriptor->state = SCHEDULER_TASK_CONSTRUCTING;
        handle->index = (uint32_t)index;
        handle->generation = descriptor->generation;
        return SCHEDULER_CORE_OK;
    }

    return retired_seen
        ? SCHEDULER_CORE_GENERATION_EXHAUSTED
        : SCHEDULER_CORE_DESCRIPTOR_LIMIT;
}

enum scheduler_core_status scheduler_core_publish_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
)
{
    struct scheduler_core_identity identity;
    enum scheduler_task_state task_state;
    enum scheduler_core_status status = scheduler_core_resolve_handle(
        state,
        handle,
        &task_state
    );

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (task_state != SCHEDULER_TASK_CONSTRUCTING) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    if (state->successful_creations == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    identity = dynamic_identity(handle.index, handle.generation);
    status = queue_push(state, identity);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++state->successful_creations;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_abort_create(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
)
{
    enum scheduler_task_state task_state;
    enum scheduler_core_status status = scheduler_core_resolve_handle(
        state,
        handle,
        &task_state
    );

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (task_state != SCHEDULER_TASK_CONSTRUCTING) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    if (state->failed_creations == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    state->descriptors[handle.index].state = SCHEDULER_TASK_UNUSED;
    ++state->failed_creations;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_yield(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    enum scheduler_core_status status;

    if (previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    identity_clear(previous);
    identity_clear(next);
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    if (state->context_switches == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    *previous = state->current;
    status = queue_push(state, *previous);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    status = queue_pop(state, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++state->context_switches;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_exit_current(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    enum scheduler_core_status status;

    if (previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    identity_clear(previous);
    identity_clear(next);
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    if (state->completed_tasks == SIZE_MAX ||
        state->context_switches == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    *previous = state->current;
    state->descriptors[previous->index].state = SCHEDULER_TASK_EXITED;
    status = queue_pop(state, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++state->completed_tasks;
    ++state->context_switches;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_begin_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
)
{
    enum scheduler_task_state task_state;
    enum scheduler_core_status status = scheduler_core_resolve_handle(
        state,
        handle,
        &task_state
    );

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    switch (task_state) {
    case SCHEDULER_TASK_EXITED:
        state->descriptors[handle.index].state = SCHEDULER_TASK_REAPING;
        return scheduler_core_validate(state);
    case SCHEDULER_TASK_RUNNING:
        return SCHEDULER_CORE_RUNNING_TASK;
    case SCHEDULER_TASK_READY:
        return SCHEDULER_CORE_RUNNABLE_TASK;
    case SCHEDULER_TASK_UNUSED:
    case SCHEDULER_TASK_RETIRED:
        return SCHEDULER_CORE_DOUBLE_REAP;
    default:
        return SCHEDULER_CORE_INVALID_STATE;
    }
}

enum scheduler_core_status scheduler_core_finish_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
)
{
    enum scheduler_task_state task_state;
    enum scheduler_core_status status = scheduler_core_resolve_handle(
        state,
        handle,
        &task_state
    );

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (task_state != SCHEDULER_TASK_REAPING) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    if (state->reaped_tasks == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    if (state->descriptors[handle.index].generation == UINT64_MAX) {
        state->descriptors[handle.index].state = SCHEDULER_TASK_RETIRED;
    } else {
        ++state->descriptors[handle.index].generation;
        state->descriptors[handle.index].state = SCHEDULER_TASK_UNUSED;
    }

    ++state->reaped_tasks;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_abort_reap(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle
)
{
    enum scheduler_task_state task_state;
    enum scheduler_core_status status = scheduler_core_resolve_handle(
        state,
        handle,
        &task_state
    );

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (task_state != SCHEDULER_TASK_REAPING) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    state->descriptors[handle.index].state = SCHEDULER_TASK_EXITED;
    return scheduler_core_validate(state);
}

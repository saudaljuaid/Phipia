/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scheduler_core.h"

_Static_assert(sizeof(struct scheduler_core_descriptor) == 64U,
    "scheduler core descriptor size changed");
_Static_assert(sizeof(struct scheduler_core_state) == 1440U,
    "scheduler core state must preserve its runtime ABI size");

static void identity_clear(struct scheduler_core_identity *identity)
{
    identity->bootstrap = false;
    identity->index = SCHEDULER_INVALID_INDEX;
    identity->generation = 0U;
}

static bool priority_is_valid(uint8_t priority)
{
    return priority <= SCHEDULER_PRIORITY_MAX;
}

static void descriptor_runtime_clear(
    struct scheduler_core_descriptor *descriptor
)
{
    descriptor->priority = SCHEDULER_PRIORITY_DEFAULT;
    descriptor->wait_age = 0U;
    descriptor->queued = false;
    descriptor->wait_event = SCHEDULER_CORE_EVENT_NONE;
    descriptor->wait_epoch = 0U;
    descriptor->wake_deadline = 0U;
    descriptor->join_generation = 0U;
    descriptor->join_completion_generation = 0U;
    descriptor->join_index = SCHEDULER_INVALID_INDEX;
    descriptor->join_completion_index = SCHEDULER_INVALID_INDEX;
    descriptor->cancel_requested = false;
    descriptor->join_completion_ready = false;

    for (size_t index = 0U; index < sizeof(descriptor->reserved); ++index) {
        descriptor->reserved[index] = 0U;
    }
}

static void descriptor_wait_clear(
    struct scheduler_core_descriptor *descriptor
)
{
    descriptor->wait_event = SCHEDULER_CORE_EVENT_NONE;
    descriptor->wait_epoch = 0U;
    descriptor->wake_deadline = 0U;
    descriptor->join_index = SCHEDULER_INVALID_INDEX;
    descriptor->join_generation = 0U;
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

static size_t queue_position(
    const struct scheduler_core_state *state,
    size_t offset
)
{
    size_t position = state->queue_head + offset;

    if (position >= SCHEDULER_CORE_QUEUE_CAPACITY) {
        position -= SCHEDULER_CORE_QUEUE_CAPACITY;
    }

    return position;
}

static void queue_entry_clear(
    struct scheduler_core_state *state,
    size_t position
)
{
    state->ready_identities[position] = SCHEDULER_CORE_QUEUE_UNUSED;
    state->ready_generations[position] = 0U;
}

static bool queue_entry_is_clear(
    const struct scheduler_core_state *state,
    size_t position
)
{
    return state->ready_identities[position] == SCHEDULER_CORE_QUEUE_UNUSED &&
        state->ready_generations[position] == 0U;
}

static void queue_entry_write(
    struct scheduler_core_state *state,
    size_t position,
    struct scheduler_core_identity identity
)
{
    state->ready_identities[position] = identity.bootstrap
        ? SCHEDULER_CORE_QUEUE_BOOTSTRAP
        : (uint8_t)identity.index;
    state->ready_generations[position] = identity.generation;
}

static bool queue_entry_read(
    const struct scheduler_core_state *state,
    size_t position,
    struct scheduler_core_identity *identity
)
{
    const uint8_t encoded = state->ready_identities[position];

    if (identity == NULL || encoded == SCHEDULER_CORE_QUEUE_UNUSED) {
        return false;
    }

    if (encoded == SCHEDULER_CORE_QUEUE_BOOTSTRAP) {
        if (state->ready_generations[position] != 0U) {
            return false;
        }

        *identity = bootstrap_identity();
        return true;
    }

    if ((size_t)encoded >= SCHEDULER_TASK_LIMIT ||
        state->ready_generations[position] == 0U) {
        return false;
    }

    *identity = dynamic_identity(
        (uint32_t)encoded,
        state->ready_generations[position]
    );
    return true;
}

static uint8_t effective_priority(
    const struct scheduler_core_state *state,
    const struct scheduler_core_identity *identity
)
{
    uint8_t priority;
    uint8_t age;
    uint8_t boost;

    if (identity->bootstrap) {
        priority = SCHEDULER_PRIORITY_DEFAULT;
        age = state->bootstrap_wait_age;
    } else {
        priority = state->descriptors[identity->index].priority;
        age = state->descriptors[identity->index].wait_age;
    }

    boost = (uint8_t)(age / SCHEDULER_CORE_AGING_INTERVAL);

    if (boost > SCHEDULER_PRIORITY_MAX - priority) {
        return SCHEDULER_PRIORITY_MAX;
    }

    return (uint8_t)(priority + boost);
}

static void age_ready_tasks(struct scheduler_core_state *state)
{
    for (size_t offset = 0U; offset < state->queue_count; ++offset) {
        struct scheduler_core_identity identity;
        const size_t position = queue_position(state, offset);
        uint8_t *age;

        if (!queue_entry_read(state, position, &identity)) {
            continue;
        }

        age = identity.bootstrap
            ? &state->bootstrap_wait_age
            : &state->descriptors[identity.index].wait_age;

        if (*age < SCHEDULER_CORE_MAXIMUM_WAIT_AGE) {
            ++*age;
        }
    }
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

    tail = queue_position(state, state->queue_count);

    if (!queue_entry_is_clear(state, tail)) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    queue_entry_write(state, tail, identity);
    ++state->queue_count;

    if (identity.bootstrap) {
        state->bootstrap_state = SCHEDULER_TASK_READY;
        state->bootstrap_wait_age = 0U;
    } else {
        struct scheduler_core_descriptor *descriptor =
            &state->descriptors[identity.index];

        descriptor->state = SCHEDULER_TASK_READY;
        descriptor->wait_age = 0U;
        descriptor->queued = true;
        descriptor_wait_clear(descriptor);
    }

    return SCHEDULER_CORE_OK;
}

static enum scheduler_core_status queue_select(
    struct scheduler_core_state *state,
    struct scheduler_core_identity *identity
)
{
    size_t selected_offset = 0U;
    uint8_t selected_priority = 0U;

    if (identity == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    for (size_t offset = 0U; offset < state->queue_count; ++offset) {
        struct scheduler_core_identity candidate;
        const size_t position = queue_position(state, offset);
        uint8_t candidate_priority;

        if (!queue_entry_read(state, position, &candidate) ||
            !identity_is_valid(state, &candidate, SCHEDULER_TASK_READY)) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        candidate_priority = effective_priority(state, &candidate);

        if (offset == 0U || candidate_priority > selected_priority) {
            selected_offset = offset;
            selected_priority = candidate_priority;
            *identity = candidate;
        }
    }

    if (selected_offset == 0U) {
        queue_entry_clear(state, state->queue_head);
        ++state->queue_head;

        if (state->queue_head == SCHEDULER_CORE_QUEUE_CAPACITY) {
            state->queue_head = 0U;
        }
    } else {
        for (size_t offset = selected_offset;
             offset + 1U < state->queue_count;
             ++offset) {
            const size_t destination = queue_position(state, offset);
            const size_t source = queue_position(state, offset + 1U);

            state->ready_identities[destination] =
                state->ready_identities[source];
            state->ready_generations[destination] =
                state->ready_generations[source];
        }

        queue_entry_clear(
            state,
            queue_position(state, state->queue_count - 1U)
        );
    }

    --state->queue_count;

    if (identity->bootstrap) {
        state->bootstrap_state = SCHEDULER_TASK_RUNNING;
        state->bootstrap_wait_age = 0U;
    } else {
        state->descriptors[identity->index].state = SCHEDULER_TASK_RUNNING;
        state->descriptors[identity->index].wait_age = 0U;
        state->descriptors[identity->index].queued = false;
    }

    state->current = *identity;
    age_ready_tasks(state);
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
        descriptor_runtime_clear(&state->descriptors[index]);
    }

    for (size_t index = 0U;
         index < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++index) {
        queue_entry_clear(state, index);
    }

    for (size_t index = 0U; index < SCHEDULER_CORE_EVENT_LIMIT; ++index) {
        state->event_epochs[index] = 1U;
        state->event_waiters[index] = 0U;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        state->join_waiters[index] = 0U;
    }

    identity_clear(&state->current);
    state->monotonic_tick = 0U;
    state->queue_head = 0U;
    state->queue_count = 0U;
    state->successful_creations = 0U;
    state->failed_creations = 0U;
    state->context_switches = 0U;
    state->completed_tasks = 0U;
    state->reaped_tasks = 0U;
    state->blocked_transitions = 0U;
    state->wakeups = 0U;
    state->join_blocks = 0U;
    state->timed_sleeps = 0U;
    state->cancellation_requests = 0U;
    state->cancellations = 0U;
    state->bootstrap_state = SCHEDULER_TASK_UNUSED;
    state->bootstrap_wait_age = 0U;
    state->initialized = false;
    state->poisoned = false;

    for (size_t index = 0U; index < sizeof(state->reserved); ++index) {
        state->reserved[index] = 0U;
    }
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
        destination->ready_generations[index] =
            source->ready_generations[index];
        destination->ready_identities[index] =
            source->ready_identities[index];
    }

    for (size_t index = 0U; index < SCHEDULER_CORE_EVENT_LIMIT; ++index) {
        destination->event_epochs[index] = source->event_epochs[index];
        destination->event_waiters[index] = source->event_waiters[index];
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        destination->join_waiters[index] = source->join_waiters[index];
    }

    destination->current = source->current;
    destination->monotonic_tick = source->monotonic_tick;
    destination->queue_head = source->queue_head;
    destination->queue_count = source->queue_count;
    destination->successful_creations = source->successful_creations;
    destination->failed_creations = source->failed_creations;
    destination->context_switches = source->context_switches;
    destination->completed_tasks = source->completed_tasks;
    destination->reaped_tasks = source->reaped_tasks;
    destination->blocked_transitions = source->blocked_transitions;
    destination->wakeups = source->wakeups;
    destination->join_blocks = source->join_blocks;
    destination->timed_sleeps = source->timed_sleeps;
    destination->cancellation_requests = source->cancellation_requests;
    destination->cancellations = source->cancellations;
    destination->bootstrap_state = source->bootstrap_state;
    destination->bootstrap_wait_age = source->bootstrap_wait_age;
    destination->initialized = source->initialized;
    destination->poisoned = source->poisoned;

    for (size_t index = 0U; index < sizeof(destination->reserved); ++index) {
        destination->reserved[index] = source->reserved[index];
    }
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

static bool descriptor_is_incomplete(
    const struct scheduler_core_descriptor *descriptor
)
{
    return descriptor->state == SCHEDULER_TASK_READY ||
        descriptor->state == SCHEDULER_TASK_RUNNING ||
        descriptor->state == SCHEDULER_TASK_BLOCKED ||
        descriptor->state == SCHEDULER_TASK_JOIN_BLOCKED ||
        descriptor->state == SCHEDULER_TASK_SLEEPING;
}

static bool join_chain_is_acyclic(
    const struct scheduler_core_state *state,
    size_t start
)
{
    size_t current = start;

    for (size_t depth = 0U; depth < SCHEDULER_TASK_LIMIT; ++depth) {
        const struct scheduler_core_descriptor *descriptor =
            &state->descriptors[current];

        if (descriptor->state != SCHEDULER_TASK_JOIN_BLOCKED) {
            return true;
        }

        if ((size_t)descriptor->join_index >= SCHEDULER_TASK_LIMIT) {
            return false;
        }

        current = descriptor->join_index;

        if (current == start) {
            return false;
        }
    }

    return false;
}

enum scheduler_core_status scheduler_core_validate(
    const struct scheduler_core_state *state
)
{
    bool observed_queue[SCHEDULER_TASK_LIMIT] = {false};
    uint16_t observed_event_waiters[SCHEDULER_CORE_EVENT_LIMIT] = {0U};
    uint16_t observed_join_waiters[SCHEDULER_TASK_LIMIT] = {0U};
    bool bootstrap_queued = false;
    size_t blocked_count = 0U;
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
        state->blocked_transitions < state->wakeups ||
        state->join_blocks > state->blocked_transitions ||
        state->timed_sleeps > state->blocked_transitions ||
        state->cancellations > state->cancellation_requests ||
        state->cancellations > state->completed_tasks ||
        state->reaped_tasks > state->completed_tasks ||
        state->completed_tasks > state->successful_creations ||
        !identity_is_valid(
            state,
            &state->current,
            SCHEDULER_TASK_RUNNING
        )) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    if (state->bootstrap_state == SCHEDULER_TASK_RUNNING) {
        if (state->bootstrap_wait_age != 0U) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        ++running_count;
    } else if (state->bootstrap_state == SCHEDULER_TASK_READY) {
        if (state->bootstrap_wait_age > SCHEDULER_CORE_MAXIMUM_WAIT_AGE) {
            return SCHEDULER_CORE_CORRUPTED;
        }
    } else {
        return SCHEDULER_CORE_CORRUPTED;
    }

    for (size_t offset = 0U;
         offset < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++offset) {
        const size_t position = queue_position(state, offset);
        bool active = offset < state->queue_count;
        struct scheduler_core_identity identity;

        if (!active) {
            if (!queue_entry_is_clear(state, position)) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            continue;
        }

        if (!queue_entry_read(state, position, &identity) ||
            !identity_is_valid(state, &identity, SCHEDULER_TASK_READY)) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        if (identity.bootstrap) {
            if (bootstrap_queued) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            bootstrap_queued = true;
        } else {
            if (observed_queue[identity.index]) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            observed_queue[identity.index] = true;
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
            !priority_is_valid(descriptor->priority) ||
            descriptor->queued != observed_queue[index]) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        for (size_t reserved = 0U;
             reserved < sizeof(descriptor->reserved);
             ++reserved) {
            if (descriptor->reserved[reserved] != 0U) {
                return SCHEDULER_CORE_CORRUPTED;
            }
        }

        if (descriptor->join_completion_ready) {
            if (descriptor->join_completion_index >= SCHEDULER_TASK_LIMIT ||
                descriptor->join_completion_generation == 0U ||
                (descriptor->state != SCHEDULER_TASK_READY &&
                 descriptor->state != SCHEDULER_TASK_RUNNING)) {
                return SCHEDULER_CORE_CORRUPTED;
            }
        } else if (descriptor->join_completion_index !=
                       SCHEDULER_INVALID_INDEX ||
                   descriptor->join_completion_generation != 0U) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        if (descriptor->cancel_requested &&
            !descriptor_is_incomplete(descriptor)) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        if (descriptor->state == SCHEDULER_TASK_BLOCKED) {
            const uint16_t waiter_bit = (uint16_t)(UINT16_C(1) << index);

            if (descriptor->queued || descriptor->wait_age != 0U ||
                descriptor->wait_event >= SCHEDULER_CORE_EVENT_LIMIT ||
                descriptor->wait_epoch !=
                    state->event_epochs[descriptor->wait_event] ||
                descriptor->wake_deadline != 0U ||
                descriptor->join_index != SCHEDULER_INVALID_INDEX ||
                descriptor->join_generation != 0U ||
                (observed_event_waiters[descriptor->wait_event] & waiter_bit) !=
                    0U) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            observed_event_waiters[descriptor->wait_event] |= waiter_bit;
            ++blocked_count;
        } else if (descriptor->state == SCHEDULER_TASK_JOIN_BLOCKED) {
            const uint16_t waiter_bit = (uint16_t)(UINT16_C(1) << index);

            if (descriptor->queued || descriptor->wait_age != 0U ||
                descriptor->wait_event != SCHEDULER_CORE_EVENT_NONE ||
                descriptor->wait_epoch != 0U ||
                descriptor->wake_deadline != 0U ||
                descriptor->join_index >= SCHEDULER_TASK_LIMIT ||
                descriptor->join_generation == 0U ||
                descriptor->join_index == index ||
                state->descriptors[descriptor->join_index].generation !=
                    descriptor->join_generation ||
                !descriptor_is_incomplete(
                    &state->descriptors[descriptor->join_index]
                ) ||
                (observed_join_waiters[descriptor->join_index] &
                    waiter_bit) != 0U) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            observed_join_waiters[descriptor->join_index] |= waiter_bit;
            ++blocked_count;
        } else if (descriptor->state == SCHEDULER_TASK_SLEEPING) {
            if (descriptor->queued || descriptor->wait_age != 0U ||
                descriptor->wait_event != SCHEDULER_CORE_EVENT_NONE ||
                descriptor->wait_epoch != 0U ||
                descriptor->wake_deadline <= state->monotonic_tick ||
                descriptor->join_index != SCHEDULER_INVALID_INDEX ||
                descriptor->join_generation != 0U) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            ++blocked_count;
        } else if (descriptor->wait_event != SCHEDULER_CORE_EVENT_NONE ||
                   descriptor->wait_epoch != 0U ||
                   descriptor->wake_deadline != 0U ||
                   descriptor->join_index != SCHEDULER_INVALID_INDEX ||
                   descriptor->join_generation != 0U) {
            return SCHEDULER_CORE_CORRUPTED;
        }

        switch (descriptor->state) {
        case SCHEDULER_TASK_UNUSED:
            if (descriptor->queued || descriptor->wait_age != 0U ||
                descriptor->priority != SCHEDULER_PRIORITY_DEFAULT ||
                descriptor->cancel_requested ||
                descriptor->join_completion_ready) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_CONSTRUCTING:
        case SCHEDULER_TASK_EXITED:
        case SCHEDULER_TASK_REAPING:
            if (descriptor->queued || descriptor->wait_age != 0U ||
                descriptor->cancel_requested ||
                descriptor->join_completion_ready) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_READY:
            if (!descriptor->queued ||
                descriptor->wait_age > SCHEDULER_CORE_MAXIMUM_WAIT_AGE) {
                return SCHEDULER_CORE_CORRUPTED;
            }
            break;
        case SCHEDULER_TASK_RUNNING:
            if (descriptor->queued || descriptor->wait_age != 0U) {
                return SCHEDULER_CORE_CORRUPTED;
            }

            ++running_count;
            break;
        case SCHEDULER_TASK_BLOCKED:
        case SCHEDULER_TASK_JOIN_BLOCKED:
        case SCHEDULER_TASK_SLEEPING:
            break;
        case SCHEDULER_TASK_RETIRED:
            if (descriptor->generation != UINT64_MAX || descriptor->queued ||
                descriptor->wait_age != 0U ||
                descriptor->priority != SCHEDULER_PRIORITY_DEFAULT ||
                descriptor->cancel_requested ||
                descriptor->join_completion_ready) {
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

    if (blocked_count != state->blocked_transitions - state->wakeups) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    for (size_t index = 0U; index < SCHEDULER_CORE_EVENT_LIMIT; ++index) {
        if (state->event_epochs[index] == 0U ||
            state->event_waiters[index] != observed_event_waiters[index]) {
            return SCHEDULER_CORE_CORRUPTED;
        }
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        if (state->join_waiters[index] != observed_join_waiters[index] ||
            !join_chain_is_acyclic(state, index)) {
            return SCHEDULER_CORE_CORRUPTED;
        }
    }

    for (size_t index = 0U; index < sizeof(state->reserved); ++index) {
        if (state->reserved[index] != 0U) {
            return SCHEDULER_CORE_CORRUPTED;
        }
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
    return scheduler_core_begin_create_with_priority(
        state,
        SCHEDULER_PRIORITY_DEFAULT,
        handle
    );
}

enum scheduler_core_status scheduler_core_begin_create_with_priority(
    struct scheduler_core_state *state,
    uint8_t priority,
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

    if (!priority_is_valid(priority)) {
        return SCHEDULER_CORE_INVALID_PRIORITY;
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
        descriptor->priority = priority;
        descriptor->wait_age = 0U;
        descriptor->queued = false;
        descriptor->wait_event = SCHEDULER_CORE_EVENT_NONE;
        descriptor->wait_epoch = 0U;
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
    descriptor_runtime_clear(&state->descriptors[handle.index]);
    ++state->failed_creations;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_set_priority(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    uint8_t priority
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

    if (!priority_is_valid(priority)) {
        return SCHEDULER_CORE_INVALID_PRIORITY;
    }

    switch (task_state) {
    case SCHEDULER_TASK_CONSTRUCTING:
    case SCHEDULER_TASK_READY:
    case SCHEDULER_TASK_RUNNING:
    case SCHEDULER_TASK_BLOCKED:
    case SCHEDULER_TASK_JOIN_BLOCKED:
    case SCHEDULER_TASK_SLEEPING:
        state->descriptors[handle.index].priority = priority;
        state->descriptors[handle.index].wait_age = 0U;
        return scheduler_core_validate(state);
    default:
        return SCHEDULER_CORE_INVALID_STATE;
    }
}

static void descriptor_completion_clear(
    struct scheduler_core_descriptor *descriptor
)
{
    descriptor->join_completion_index = SCHEDULER_INVALID_INDEX;
    descriptor->join_completion_generation = 0U;
    descriptor->join_completion_ready = false;
}

static enum scheduler_core_status complete_current(
    struct scheduler_core_state *state,
    bool cancelled,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    struct scheduler_core_state candidate;
    struct scheduler_core_descriptor *completed;
    enum scheduler_core_status status;
    uint16_t waiters;
    size_t waking = 0U;

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    waiters = state->join_waiters[state->current.index];

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        if ((waiters & (uint16_t)(UINT16_C(1) << index)) != 0U) {
            ++waking;
        }
    }

    if (state->completed_tasks == SIZE_MAX ||
        state->context_switches == SIZE_MAX ||
        state->wakeups > SIZE_MAX - waking ||
        (cancelled && state->cancellations == SIZE_MAX)) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    if (waking > SCHEDULER_CORE_QUEUE_CAPACITY ||
        state->queue_count > SCHEDULER_CORE_QUEUE_CAPACITY - waking) {
        return SCHEDULER_CORE_QUEUE_LIMIT;
    }

    scheduler_core_copy(&candidate, state);
    *previous = candidate.current;
    completed = &candidate.descriptors[previous->index];
    completed->state = SCHEDULER_TASK_EXITED;
    completed->wait_age = 0U;
    completed->queued = false;
    completed->cancel_requested = false;
    descriptor_wait_clear(completed);
    descriptor_completion_clear(completed);

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const uint16_t waiter_bit = (uint16_t)(UINT16_C(1) << index);
        struct scheduler_core_descriptor *waiter;
        struct scheduler_core_identity identity;

        if ((waiters & waiter_bit) == 0U) {
            continue;
        }

        waiter = &candidate.descriptors[index];
        candidate.join_waiters[previous->index] &=
            (uint16_t)~waiter_bit;
        descriptor_wait_clear(waiter);
        waiter->join_completion_index = previous->index;
        waiter->join_completion_generation = previous->generation;
        waiter->join_completion_ready = true;
        identity = dynamic_identity((uint32_t)index, waiter->generation);
        status = queue_push(&candidate, identity);

        if (status != SCHEDULER_CORE_OK) {
            return status;
        }
    }

    status = queue_select(&candidate, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++candidate.completed_tasks;
    ++candidate.context_switches;
    candidate.wakeups += waking;

    if (cancelled) {
        ++candidate.cancellations;
    }

    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    return SCHEDULER_CORE_OK;
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

    status = queue_select(state, next);

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

    return complete_current(state, false, previous, next);
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
    case SCHEDULER_TASK_BLOCKED:
    case SCHEDULER_TASK_JOIN_BLOCKED:
    case SCHEDULER_TASK_SLEEPING:
        return SCHEDULER_CORE_BLOCKED_TASK;
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

    descriptor_runtime_clear(&state->descriptors[handle.index]);

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

enum scheduler_core_status scheduler_core_event_observe(
    const struct scheduler_core_state *state,
    uint32_t event_index,
    struct scheduler_core_event_token *token
)
{
    enum scheduler_core_status status;

    if (token == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    token->index = UINT32_MAX;
    token->epoch = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if ((size_t)event_index >= SCHEDULER_CORE_EVENT_LIMIT) {
        return SCHEDULER_CORE_INVALID_EVENT;
    }

    token->index = event_index;
    token->epoch = state->event_epochs[event_index];
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_block_current(
    struct scheduler_core_state *state,
    struct scheduler_core_event_token token,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    struct scheduler_core_descriptor *descriptor;
    enum scheduler_core_status status;
    uint16_t waiter_bit;

    if (previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    identity_clear(previous);
    identity_clear(next);
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if ((size_t)token.index >= SCHEDULER_CORE_EVENT_LIMIT) {
        return SCHEDULER_CORE_INVALID_EVENT;
    }

    if (state->event_epochs[token.index] == UINT64_MAX) {
        return SCHEDULER_CORE_EVENT_EPOCH_EXHAUSTED;
    }

    if (state->event_epochs[token.index] != token.epoch) {
        return SCHEDULER_CORE_EVENT_CHANGED;
    }

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    if (state->descriptors[state->current.index].join_completion_ready) {
        return SCHEDULER_CORE_JOIN_COMPLETION_PENDING;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    if (state->blocked_transitions == SIZE_MAX ||
        state->context_switches == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    descriptor = &state->descriptors[state->current.index];
    waiter_bit = (uint16_t)(UINT16_C(1) << state->current.index);

    if ((state->event_waiters[token.index] & waiter_bit) != 0U) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    *previous = state->current;
    descriptor->state = SCHEDULER_TASK_BLOCKED;
    descriptor->wait_age = 0U;
    descriptor->queued = false;
    descriptor->wait_event = (uint8_t)token.index;
    descriptor->wait_epoch = token.epoch;
    state->event_waiters[token.index] |= waiter_bit;
    status = queue_select(state, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++state->blocked_transitions;
    ++state->context_switches;
    return scheduler_core_validate(state);
}

enum scheduler_core_status scheduler_core_event_signal(
    struct scheduler_core_state *state,
    uint32_t event_index,
    bool wake_all,
    size_t *woken_count,
    uint64_t *new_epoch
)
{
    struct scheduler_core_state candidate;
    enum scheduler_core_status status;
    uint16_t waiters;
    size_t wake_limit;
    size_t waking = 0U;
    size_t woken = 0U;

    if (woken_count == NULL || new_epoch == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *woken_count = 0U;
    *new_epoch = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if ((size_t)event_index >= SCHEDULER_CORE_EVENT_LIMIT) {
        return SCHEDULER_CORE_INVALID_EVENT;
    }

    if (state->event_epochs[event_index] == UINT64_MAX) {
        return SCHEDULER_CORE_EVENT_EPOCH_EXHAUSTED;
    }

    waiters = state->event_waiters[event_index];
    wake_limit = wake_all ? SCHEDULER_TASK_LIMIT : (size_t)1U;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        if ((waiters & (uint16_t)(UINT16_C(1) << index)) != 0U &&
            waking < wake_limit) {
            ++waking;
        }
    }

    if (state->wakeups > SIZE_MAX - waking) {
        return SCHEDULER_CORE_STATS_INVALID;
    }
    if (waking > SCHEDULER_CORE_QUEUE_CAPACITY ||
        state->queue_count > SCHEDULER_CORE_QUEUE_CAPACITY - waking) {
        return SCHEDULER_CORE_QUEUE_LIMIT;
    }

    scheduler_core_copy(&candidate, state);
    ++candidate.event_epochs[event_index];

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        struct scheduler_core_descriptor *descriptor =
            &candidate.descriptors[index];
        const uint16_t waiter_bit = (uint16_t)(UINT16_C(1) << index);

        if ((waiters & waiter_bit) == 0U) {
            continue;
        }

        if (woken < wake_limit) {
            struct scheduler_core_identity identity = dynamic_identity(
                (uint32_t)index,
                descriptor->generation
            );

            candidate.event_waiters[event_index] &=
                (uint16_t)~waiter_bit;
            status = queue_push(&candidate, identity);

            if (status != SCHEDULER_CORE_OK) {
                return status;
            }

            ++woken;
        } else {
            descriptor->wait_epoch = candidate.event_epochs[event_index];
        }
    }

    candidate.wakeups += woken;
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    *woken_count = woken;
    *new_epoch = state->event_epochs[event_index];
    return SCHEDULER_CORE_OK;
}

static bool join_would_cycle(
    const struct scheduler_core_state *state,
    uint32_t waiter_index,
    uint32_t target_index
)
{
    uint32_t current = target_index;

    for (size_t depth = 0U; depth < SCHEDULER_TASK_LIMIT; ++depth) {
        const struct scheduler_core_descriptor *descriptor;

        if (current == waiter_index) {
            return true;
        }

        descriptor = &state->descriptors[current];

        if (descriptor->state != SCHEDULER_TASK_JOIN_BLOCKED) {
            return false;
        }

        current = descriptor->join_index;
    }

    return true;
}

enum scheduler_core_status scheduler_core_join_current(
    struct scheduler_core_state *state,
    struct scheduler_task_handle target,
    bool *completed,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    struct scheduler_core_state candidate;
    struct scheduler_core_descriptor *waiter;
    enum scheduler_task_state target_state;
    enum scheduler_core_status status;
    uint16_t waiter_bit;

    if (completed == NULL || previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *completed = false;
    identity_clear(previous);
    identity_clear(next);
    status = scheduler_core_resolve_handle(state, target, &target_state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    waiter = &state->descriptors[state->current.index];

    if (waiter->join_completion_ready) {
        return SCHEDULER_CORE_JOIN_COMPLETION_PENDING;
    }

    if (target.index == state->current.index &&
        target.generation == state->current.generation) {
        return SCHEDULER_CORE_SELF_JOIN;
    }

    if (target_state == SCHEDULER_TASK_EXITED ||
        target_state == SCHEDULER_TASK_REAPING) {
        scheduler_core_copy(&candidate, state);
        waiter = &candidate.descriptors[candidate.current.index];
        waiter->join_completion_index = target.index;
        waiter->join_completion_generation = target.generation;
        waiter->join_completion_ready = true;
        status = scheduler_core_validate(&candidate);

        if (status != SCHEDULER_CORE_OK) {
            return status;
        }

        scheduler_core_copy(state, &candidate);
        *completed = true;
        return SCHEDULER_CORE_OK;
    }

    if (!descriptor_is_incomplete(&state->descriptors[target.index])) {
        return target_state == SCHEDULER_TASK_UNUSED ||
            target_state == SCHEDULER_TASK_RETIRED
            ? SCHEDULER_CORE_DOUBLE_REAP
            : SCHEDULER_CORE_INVALID_STATE;
    }

    if (join_would_cycle(state, state->current.index, target.index)) {
        return SCHEDULER_CORE_JOIN_CYCLE;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    if (state->blocked_transitions == SIZE_MAX ||
        state->join_blocks == SIZE_MAX ||
        state->context_switches == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    scheduler_core_copy(&candidate, state);
    *previous = candidate.current;
    waiter = &candidate.descriptors[previous->index];
    waiter_bit = (uint16_t)(UINT16_C(1) << previous->index);

    if ((candidate.join_waiters[target.index] & waiter_bit) != 0U) {
        return SCHEDULER_CORE_CORRUPTED;
    }

    waiter->state = SCHEDULER_TASK_JOIN_BLOCKED;
    waiter->wait_age = 0U;
    waiter->queued = false;
    descriptor_wait_clear(waiter);
    waiter->join_index = target.index;
    waiter->join_generation = target.generation;
    candidate.join_waiters[target.index] |= waiter_bit;
    status = queue_select(&candidate, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++candidate.blocked_transitions;
    ++candidate.join_blocks;
    ++candidate.context_switches;
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_consume_join_completion(
    struct scheduler_core_state *state,
    struct scheduler_task_handle *completion
)
{
    struct scheduler_core_state candidate;
    struct scheduler_core_descriptor *descriptor;
    enum scheduler_core_status status;

    if (completion == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    completion->index = SCHEDULER_INVALID_INDEX;
    completion->generation = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    descriptor = &state->descriptors[state->current.index];

    if (!descriptor->join_completion_ready) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    *completion = (struct scheduler_task_handle) {
        .index = descriptor->join_completion_index,
        .generation = descriptor->join_completion_generation
    };
    scheduler_core_copy(&candidate, state);
    descriptor_completion_clear(
        &candidate.descriptors[candidate.current.index]
    );
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        completion->index = SCHEDULER_INVALID_INDEX;
        completion->generation = 0U;
        return status;
    }

    scheduler_core_copy(state, &candidate);
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_request_cancel(
    struct scheduler_core_state *state,
    struct scheduler_task_handle handle,
    bool *made_runnable
)
{
    struct scheduler_core_state candidate;
    struct scheduler_core_descriptor *descriptor;
    struct scheduler_core_identity identity;
    enum scheduler_task_state task_state;
    enum scheduler_core_status status;
    uint16_t task_bit;
    bool blocked;

    if (made_runnable == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *made_runnable = false;
    status = scheduler_core_resolve_handle(state, handle, &task_state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (!descriptor_is_incomplete(&state->descriptors[handle.index])) {
        return SCHEDULER_CORE_INVALID_STATE;
    }

    if (state->descriptors[handle.index].cancel_requested) {
        return SCHEDULER_CORE_OK;
    }

    blocked = task_state == SCHEDULER_TASK_BLOCKED ||
        task_state == SCHEDULER_TASK_JOIN_BLOCKED ||
        task_state == SCHEDULER_TASK_SLEEPING;

    if (state->cancellation_requests == SIZE_MAX ||
        (blocked && state->wakeups == SIZE_MAX)) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    if (blocked && state->queue_count == SCHEDULER_CORE_QUEUE_CAPACITY) {
        return SCHEDULER_CORE_QUEUE_LIMIT;
    }

    scheduler_core_copy(&candidate, state);
    descriptor = &candidate.descriptors[handle.index];
    descriptor->cancel_requested = true;
    task_bit = (uint16_t)(UINT16_C(1) << handle.index);

    if (task_state == SCHEDULER_TASK_BLOCKED) {
        candidate.event_waiters[descriptor->wait_event] &=
            (uint16_t)~task_bit;
    } else if (task_state == SCHEDULER_TASK_JOIN_BLOCKED) {
        candidate.join_waiters[descriptor->join_index] &=
            (uint16_t)~task_bit;
    }

    if (blocked) {
        identity = dynamic_identity(handle.index, handle.generation);
        descriptor_wait_clear(descriptor);
        status = queue_push(&candidate, identity);

        if (status != SCHEDULER_CORE_OK) {
            return status;
        }

        ++candidate.wakeups;
    }

    ++candidate.cancellation_requests;
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    *made_runnable = blocked;
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_cancellation_point(
    struct scheduler_core_state *state,
    bool *cancelled,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    enum scheduler_core_status status;

    if (cancelled == NULL || previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *cancelled = false;
    identity_clear(previous);
    identity_clear(next);
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->current.bootstrap) {
        return SCHEDULER_CORE_BOOTSTRAP_TASK;
    }

    if (!state->descriptors[state->current.index].cancel_requested) {
        return SCHEDULER_CORE_CANCELLATION_NOT_REQUESTED;
    }

    status = complete_current(state, true, previous, next);

    if (status == SCHEDULER_CORE_OK) {
        *cancelled = true;
    }

    return status;
}

enum scheduler_core_status scheduler_core_sleep_current_until(
    struct scheduler_core_state *state,
    uint64_t deadline,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    struct scheduler_core_state candidate;
    struct scheduler_core_descriptor *descriptor;
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

    descriptor = &state->descriptors[state->current.index];

    if (descriptor->join_completion_ready) {
        return SCHEDULER_CORE_JOIN_COMPLETION_PENDING;
    }

    if (deadline <= state->monotonic_tick) {
        return SCHEDULER_CORE_INVALID_DEADLINE;
    }

    if (state->queue_count == 0U) {
        return SCHEDULER_CORE_NO_RUNNABLE_PEER;
    }

    if (state->blocked_transitions == SIZE_MAX ||
        state->timed_sleeps == SIZE_MAX ||
        state->context_switches == SIZE_MAX) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    scheduler_core_copy(&candidate, state);
    *previous = candidate.current;
    descriptor = &candidate.descriptors[previous->index];
    descriptor->state = SCHEDULER_TASK_SLEEPING;
    descriptor->wait_age = 0U;
    descriptor->queued = false;
    descriptor_wait_clear(descriptor);
    descriptor->wake_deadline = deadline;
    status = queue_select(&candidate, next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    ++candidate.blocked_transitions;
    ++candidate.timed_sleeps;
    ++candidate.context_switches;
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_sleep_current_for(
    struct scheduler_core_state *state,
    uint64_t duration,
    struct scheduler_core_identity *previous,
    struct scheduler_core_identity *next
)
{
    enum scheduler_core_status status = scheduler_core_validate(state);

    if (previous == NULL || next == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    identity_clear(previous);
    identity_clear(next);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (duration == 0U) {
        return SCHEDULER_CORE_INVALID_DEADLINE;
    }

    if (duration > UINT64_MAX - state->monotonic_tick) {
        return SCHEDULER_CORE_DEADLINE_OVERFLOW;
    }

    return scheduler_core_sleep_current_until(
        state,
        state->monotonic_tick + duration,
        previous,
        next
    );
}

enum scheduler_core_status scheduler_core_advance_time(
    struct scheduler_core_state *state,
    uint64_t new_tick,
    size_t *woken_count
)
{
    struct scheduler_core_state candidate;
    enum scheduler_core_status status;
    size_t waking = 0U;

    if (woken_count == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *woken_count = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (new_tick < state->monotonic_tick) {
        return SCHEDULER_CORE_TIME_REGRESSION;
    }

    if (new_tick == state->monotonic_tick) {
        return SCHEDULER_CORE_OK;
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const struct scheduler_core_descriptor *descriptor =
            &state->descriptors[index];

        if (descriptor->state == SCHEDULER_TASK_SLEEPING &&
            descriptor->wake_deadline <= new_tick) {
            ++waking;
        }
    }

    if (state->wakeups > SIZE_MAX - waking) {
        return SCHEDULER_CORE_STATS_INVALID;
    }

    if (waking > SCHEDULER_CORE_QUEUE_CAPACITY ||
        state->queue_count > SCHEDULER_CORE_QUEUE_CAPACITY - waking) {
        return SCHEDULER_CORE_QUEUE_LIMIT;
    }

    scheduler_core_copy(&candidate, state);
    candidate.monotonic_tick = new_tick;

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        struct scheduler_core_descriptor *descriptor =
            &candidate.descriptors[index];
        struct scheduler_core_identity identity;

        if (descriptor->state != SCHEDULER_TASK_SLEEPING ||
            descriptor->wake_deadline > new_tick) {
            continue;
        }

        descriptor_wait_clear(descriptor);
        identity = dynamic_identity((uint32_t)index, descriptor->generation);
        status = queue_push(&candidate, identity);

        if (status != SCHEDULER_CORE_OK) {
            return status;
        }
    }

    candidate.wakeups += waking;
    status = scheduler_core_validate(&candidate);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    scheduler_core_copy(state, &candidate);
    *woken_count = waking;
    return SCHEDULER_CORE_OK;
}

enum scheduler_core_status scheduler_core_timer_tick(
    struct scheduler_core_state *state,
    size_t *woken_count
)
{
    enum scheduler_core_status status;

    if (woken_count == NULL) {
        return SCHEDULER_CORE_NULL_ARGUMENT;
    }

    *woken_count = 0U;
    status = scheduler_core_validate(state);

    if (status != SCHEDULER_CORE_OK) {
        return status;
    }

    if (state->monotonic_tick == UINT64_MAX) {
        return SCHEDULER_CORE_TICK_EXHAUSTED;
    }

    return scheduler_core_advance_time(
        state,
        state->monotonic_tick + UINT64_C(1),
        woken_count
    );
}

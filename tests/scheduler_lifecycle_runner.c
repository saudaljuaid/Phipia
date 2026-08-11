/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "scheduler_core.h"

static struct scheduler_core_state state;

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    for (size_t byte = 0U; byte < 8U; ++byte) {
        hash ^= value & UINT64_C(0xFF);
        hash *= UINT64_C(1099511628211);
        value >>= 8U;
    }

    return hash;
}

static bool ready_identity_at(
    size_t offset,
    struct scheduler_core_identity *identity
)
{
    size_t position;
    uint8_t encoded;

    if (identity == NULL || offset >= state.queue_count ||
        state.queue_head >= SCHEDULER_CORE_QUEUE_CAPACITY) {
        return false;
    }

    position = (state.queue_head + offset) % SCHEDULER_CORE_QUEUE_CAPACITY;
    encoded = state.ready_identities[position];

    if (encoded == SCHEDULER_CORE_QUEUE_BOOTSTRAP) {
        identity->bootstrap = true;
        identity->index = SCHEDULER_INVALID_INDEX;
        identity->generation = 0U;
        return state.ready_generations[position] == 0U;
    }

    if ((size_t)encoded >= SCHEDULER_TASK_LIMIT ||
        state.ready_generations[position] == 0U) {
        return false;
    }

    identity->bootstrap = false;
    identity->index = encoded;
    identity->generation = state.ready_generations[position];
    return true;
}

static uint64_t state_hash(void)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    hash = hash_u64(hash, state.monotonic_tick);
    hash = hash_u64(hash, state.current.bootstrap ? 1U : 0U);
    hash = hash_u64(hash, state.current.index);
    hash = hash_u64(hash, state.current.generation);
    hash = hash_u64(hash, state.bootstrap_state);
    hash = hash_u64(hash, state.bootstrap_wait_age);
    hash = hash_u64(hash, state.queue_head);
    hash = hash_u64(hash, state.queue_count);

    for (size_t offset = 0U;
         offset < SCHEDULER_CORE_QUEUE_CAPACITY;
         ++offset) {
        struct scheduler_core_identity identity = {
            .bootstrap = false,
            .index = SCHEDULER_INVALID_INDEX,
            .generation = 0U
        };

        if (offset < state.queue_count &&
            !ready_identity_at(offset, &identity)) {
            identity.index = SCHEDULER_INVALID_INDEX - 1U;
        }

        hash = hash_u64(hash, identity.bootstrap ? 1U : 0U);
        hash = hash_u64(hash, identity.index);
        hash = hash_u64(hash, identity.generation);
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        const struct scheduler_core_descriptor *descriptor =
            &state.descriptors[index];

        hash = hash_u64(hash, descriptor->state);
        hash = hash_u64(hash, descriptor->generation);
        hash = hash_u64(hash, descriptor->queued ? 1U : 0U);
        hash = hash_u64(hash, descriptor->priority);
        hash = hash_u64(hash, descriptor->wait_age);
        hash = hash_u64(hash, descriptor->wait_event);
        hash = hash_u64(hash, descriptor->wait_epoch);
        hash = hash_u64(hash, descriptor->wake_deadline);
        hash = hash_u64(hash, descriptor->join_index);
        hash = hash_u64(hash, descriptor->join_generation);
        hash = hash_u64(hash, descriptor->join_completion_index);
        hash = hash_u64(hash, descriptor->join_completion_generation);
        hash = hash_u64(hash, descriptor->cancel_requested ? 1U : 0U);
        hash = hash_u64(hash, descriptor->join_completion_ready ? 1U : 0U);
    }

    for (size_t index = 0U; index < SCHEDULER_CORE_EVENT_LIMIT; ++index) {
        hash = hash_u64(hash, state.event_epochs[index]);
        hash = hash_u64(hash, state.event_waiters[index]);
    }

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        hash = hash_u64(hash, state.join_waiters[index]);
    }

    hash = hash_u64(hash, state.successful_creations);
    hash = hash_u64(hash, state.failed_creations);
    hash = hash_u64(hash, state.context_switches);
    hash = hash_u64(hash, state.completed_tasks);
    hash = hash_u64(hash, state.reaped_tasks);
    hash = hash_u64(hash, state.blocked_transitions);
    hash = hash_u64(hash, state.wakeups);
    hash = hash_u64(hash, state.join_blocks);
    hash = hash_u64(hash, state.timed_sleeps);
    hash = hash_u64(hash, state.cancellation_requests);
    hash = hash_u64(hash, state.cancellations);
    return hash;
}

static void print_result(char operation, enum scheduler_core_status status)
{
    printf("%c %u\n", operation, (unsigned int)status);
}

int main(void)
{
    char operation;

    scheduler_core_clear(&state);

    while (scanf(" %c", &operation) == 1) {
        struct scheduler_task_handle handle = {
            .index = SCHEDULER_INVALID_INDEX,
            .generation = 0U
        };
        struct scheduler_core_identity previous;
        struct scheduler_core_identity next;
        enum scheduler_core_status status;
        uint32_t index;
        uint32_t event_index;
        uint64_t value;
        uint64_t generation;
        size_t count;
        bool flag;
        unsigned int priority;

        switch (operation) {
        case 'I':
            scheduler_core_clear(&state);
            status = scheduler_core_initialize(&state);
            print_result(operation, status);
            break;
        case 'C':
            if (scanf(" %u", &priority) != 1 || priority > UINT8_MAX) {
                return 2;
            }

            status = scheduler_core_begin_create_with_priority(
                &state,
                (uint8_t)priority,
                &handle
            );

            if (status == SCHEDULER_CORE_OK) {
                status = scheduler_core_publish_create(&state, handle);
            }

            printf(
                "C %u %" PRIu32 " %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? handle.index
                    : SCHEDULER_INVALID_INDEX,
                status == SCHEDULER_CORE_OK ? handle.generation : 0U
            );
            break;
        case 'Y':
            status = scheduler_core_yield(&state, &previous, &next);
            print_result(operation, status);
            break;
        case 'E':
            status = scheduler_core_exit_current(&state, &previous, &next);
            print_result(operation, status);
            break;
        case 'J':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;
            status = scheduler_core_join_current(
                &state,
                handle,
                &flag,
                &previous,
                &next
            );
            printf(
                "J %u %u\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK && flag ? 1U : 0U
            );
            break;
        case 'M':
            status = scheduler_core_consume_join_completion(&state, &handle);
            printf(
                "M %u %" PRIu32 " %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? handle.index
                    : SCHEDULER_INVALID_INDEX,
                status == SCHEDULER_CORE_OK ? handle.generation : 0U
            );
            break;
        case 'K':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;
            status = scheduler_core_request_cancel(&state, handle, &flag);
            printf(
                "K %u %u\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK && flag ? 1U : 0U
            );
            break;
        case 'P':
            status = scheduler_core_cancellation_point(
                &state,
                &flag,
                &previous,
                &next
            );
            printf(
                "P %u %u\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK && flag ? 1U : 0U
            );
            break;
        case 'D':
        case 'F':
            if (scanf(" %" SCNu64, &value) != 1) {
                return 2;
            }

            status = operation == 'D'
                ? scheduler_core_sleep_current_until(
                    &state,
                    value,
                    &previous,
                    &next
                )
                : scheduler_core_sleep_current_for(
                    &state,
                    value,
                    &previous,
                    &next
                );
            print_result(operation, status);
            break;
        case 'B': {
            struct scheduler_core_event_token token;

            if (scanf(" %" SCNu32, &event_index) != 1) {
                return 2;
            }

            status = scheduler_core_event_observe(
                &state,
                event_index,
                &token
            );

            if (status == SCHEDULER_CORE_OK) {
                status = scheduler_core_block_current(
                    &state,
                    token,
                    &previous,
                    &next
                );
            }

            print_result(operation, status);
            break;
        }
        case 'A':
            if (scanf(" %" SCNu64, &value) != 1) {
                return 2;
            }

            status = scheduler_core_advance_time(&state, value, &count);
            printf(
                "A %u %zu\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK ? count : 0U
            );
            break;
        case 'T':
            status = scheduler_core_timer_tick(&state, &count);
            printf(
                "T %u %zu\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK ? count : 0U
            );
            break;
        case 'R':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;
            status = scheduler_core_begin_reap(&state, handle);

            if (status == SCHEDULER_CORE_OK) {
                status = scheduler_core_finish_reap(&state, handle);
            }

            print_result(operation, status);
            break;
        case 'Z':
            if (scanf(
                    " %" SCNu32 " %" SCNu64 " %u",
                    &index,
                    &generation,
                    &priority
                ) != 3 || priority > UINT8_MAX) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;
            status = scheduler_core_set_priority(
                &state,
                handle,
                (uint8_t)priority
            );
            print_result(operation, status);
            break;
        case 'G':
            if (scanf(" %" SCNu64, &value) != 1) {
                return 2;
            }

            state.monotonic_tick = value;
            status = scheduler_core_validate(&state);
            print_result(operation, status);
            break;
        case 'V':
            status = scheduler_core_validate(&state);
            print_result(operation, status);
            break;
        case 'S':
            printf("S %016" PRIx64 "\n", state_hash());
            break;
        default:
            return 2;
        }
    }

    return ferror(stdin) != 0 ? 3 : 0;
}

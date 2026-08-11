/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/scheduler_core.h"

static struct scheduler_core_state state;

static bool event_signal_boundary_checks(void)
{
    struct scheduler_core_state local = {0};
    struct scheduler_core_identity previous;
    struct scheduler_core_identity next;
    struct scheduler_core_event_token token;
    struct scheduler_task_handle handle;
    size_t woken_count = SIZE_MAX;
    uint64_t new_epoch = UINT64_MAX;

    scheduler_core_clear(&local);
    if (scheduler_core_initialize(&local) != SCHEDULER_CORE_OK) {
        return false;
    }
    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        if (scheduler_core_begin_create(&local, &handle) !=
                SCHEDULER_CORE_OK ||
            scheduler_core_publish_create(&local, handle) !=
                SCHEDULER_CORE_OK) {
            return false;
        }
    }
    if (scheduler_core_yield(&local, &previous, &next) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_event_observe(&local, 0U, &token) !=
            SCHEDULER_CORE_OK) {
        return false;
    }
    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        if (local.current.bootstrap ||
            scheduler_core_block_current(
                &local,
                token,
                &previous,
                &next
            ) != SCHEDULER_CORE_OK) {
            return false;
        }
    }
    if (!local.current.bootstrap || local.queue_count != 0U ||
        local.event_waiters[0] != UINT16_MAX ||
        scheduler_core_event_signal(
            &local,
            0U,
            true,
            &woken_count,
            &new_epoch
        ) != SCHEDULER_CORE_OK ||
        woken_count != SCHEDULER_TASK_LIMIT || new_epoch != 2U ||
        local.queue_count != SCHEDULER_TASK_LIMIT ||
        local.event_waiters[0] != 0U ||
        scheduler_core_validate(&local) != SCHEDULER_CORE_OK) {
        return false;
    }

    scheduler_core_clear(&local);
    if (scheduler_core_initialize(&local) != SCHEDULER_CORE_OK ||
        scheduler_core_begin_create(&local, &handle) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_publish_create(&local, handle) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_yield(&local, &previous, &next) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_event_observe(&local, 0U, &token) !=
            SCHEDULER_CORE_OK ||
        scheduler_core_block_current(
            &local,
            token,
            &previous,
            &next
        ) != SCHEDULER_CORE_OK) {
        return false;
    }
    local.blocked_transitions = SIZE_MAX;
    local.wakeups = SIZE_MAX - 1U;
    woken_count = SIZE_MAX;
    new_epoch = UINT64_MAX;
    return scheduler_core_event_signal(
            &local,
            0U,
            false,
            &woken_count,
            &new_epoch
        ) == SCHEDULER_CORE_OK &&
        woken_count == 1U && new_epoch == 2U &&
        local.wakeups == SIZE_MAX &&
        scheduler_core_validate(&local) == SCHEDULER_CORE_OK;
}

static void print_identity(const struct scheduler_core_identity *identity)
{
    printf(
        " %u %" PRIu32 " %" PRIu64,
        identity->bootstrap ? 1U : 0U,
        identity->index,
        identity->generation
    );
}

static bool ready_identity_at(
    size_t offset,
    struct scheduler_core_identity *identity
)
{
    size_t position;
    uint8_t encoded;

    if (state.queue_head >= SCHEDULER_CORE_QUEUE_CAPACITY ||
        offset >= SCHEDULER_CORE_QUEUE_CAPACITY) {
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

    if ((size_t)encoded >= SCHEDULER_TASK_LIMIT) {
        return false;
    }

    identity->bootstrap = false;
    identity->index = encoded;
    identity->generation = state.ready_generations[position];
    return identity->generation != 0U;
}

static void print_snapshot(void)
{
    printf(
        "S %u %u %zu %zu",
        (unsigned int)scheduler_core_validate(&state),
        (unsigned int)state.bootstrap_state,
        state.queue_head,
        state.queue_count
    );
    print_identity(&state.current);

    for (size_t offset = 0U; offset < state.queue_count; ++offset) {
        struct scheduler_core_identity identity;

        if (!ready_identity_at(offset, &identity)) {
            identity.bootstrap = false;
            identity.index = SCHEDULER_INVALID_INDEX;
            identity.generation = 0U;
        }

        print_identity(&identity);
    }

    printf(" | ");

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        printf(
            "%u:%" PRIu64 ":%u:%u:%u:%u:%" PRIu64 "%s",
            (unsigned int)state.descriptors[index].state,
            state.descriptors[index].generation,
            state.descriptors[index].queued ? 1U : 0U,
            (unsigned int)state.descriptors[index].priority,
            (unsigned int)state.descriptors[index].wait_age,
            (unsigned int)state.descriptors[index].wait_event,
            state.descriptors[index].wait_epoch,
            index + 1U == SCHEDULER_TASK_LIMIT ? "" : ","
        );
    }

    printf(" | ");

    for (size_t index = 0U; index < SCHEDULER_CORE_EVENT_LIMIT; ++index) {
        printf(
            "%" PRIu64 ":%u%s",
            state.event_epochs[index],
            (unsigned int)state.event_waiters[index],
            index + 1U == SCHEDULER_CORE_EVENT_LIMIT ? "" : ","
        );
    }

    printf(
        " | %zu %zu %zu %zu %zu %zu %zu\n",
        state.successful_creations,
        state.failed_creations,
        state.context_switches,
        state.completed_tasks,
        state.reaped_tasks,
        state.blocked_transitions,
        state.wakeups
    );
}

int main(void)
{
    char operation;

    scheduler_core_clear(&state);

    while (scanf(" %c", &operation) == 1) {
        struct scheduler_task_handle handle;
        struct scheduler_core_identity previous;
        struct scheduler_core_identity next;
        enum scheduler_task_state task_state;
        enum scheduler_core_status status;
        uint32_t index;
        uint64_t generation;
        uint32_t event_index;
        uint64_t event_epoch;
        unsigned int priority;

        switch (operation) {
        case 'I':
            scheduler_core_clear(&state);
            status = scheduler_core_initialize(&state);
            printf("I %u\n", (unsigned int)status);
            break;
        case 'J':
            status = scheduler_core_initialize(&state);
            printf("J %u\n", (unsigned int)status);
            break;
        case 'B':
            status = scheduler_core_begin_create(&state, &handle);
            printf(
                "B %u %" PRIu32 " %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? handle.index
                    : SCHEDULER_INVALID_INDEX,
                status == SCHEDULER_CORE_OK
                    ? handle.generation
                    : UINT64_C(0)
            );
            break;
        case 'P':
        case 'A':
        case 'D':
        case 'F':
        case 'X':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;

            if (operation == 'P') {
                status = scheduler_core_publish_create(&state, handle);
            } else if (operation == 'A') {
                status = scheduler_core_abort_create(&state, handle);
            } else if (operation == 'D') {
                status = scheduler_core_begin_reap(&state, handle);
            } else if (operation == 'F') {
                status = scheduler_core_finish_reap(&state, handle);
            } else {
                status = scheduler_core_abort_reap(&state, handle);
            }

            printf("%c %u\n", operation, (unsigned int)status);
            break;
        case 'C':
            status = scheduler_core_begin_create(&state, &handle);

            if (status == SCHEDULER_CORE_OK) {
                status = scheduler_core_publish_create(&state, handle);
            }

            printf(
                "C %u %" PRIu32 " %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? handle.index
                    : SCHEDULER_INVALID_INDEX,
                status == SCHEDULER_CORE_OK
                    ? handle.generation
                    : UINT64_C(0)
            );
            break;
        case 'H':
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
                "H %u %" PRIu32 " %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? handle.index
                    : SCHEDULER_INVALID_INDEX,
                status == SCHEDULER_CORE_OK
                    ? handle.generation
                    : UINT64_C(0)
            );
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
            printf("Z %u\n", (unsigned int)status);
            break;
        case 'Y':
            status = scheduler_core_yield(&state, &previous, &next);
            printf("Y %u", (unsigned int)status);

            if (status == SCHEDULER_CORE_OK) {
                print_identity(&previous);
                print_identity(&next);
            }

            putchar('\n');
            break;
        case 'E':
            status = scheduler_core_exit_current(&state, &previous, &next);
            printf("E %u", (unsigned int)status);

            if (status == SCHEDULER_CORE_OK) {
                print_identity(&previous);
                print_identity(&next);
            }

            putchar('\n');
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

            printf("R %u\n", (unsigned int)status);
            break;
        case 'Q':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2) {
                return 2;
            }

            handle.index = index;
            handle.generation = generation;
            status = scheduler_core_resolve_handle(&state, handle, &task_state);
            printf(
                "Q %u %u\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK
                    ? (unsigned int)task_state
                    : 0U
            );
            break;
        case 'O': {
            struct scheduler_core_event_token token;

            if (scanf(" %" SCNu32, &event_index) != 1) {
                return 2;
            }

            status = scheduler_core_event_observe(
                &state,
                event_index,
                &token
            );
            printf(
                "O %u %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK ? token.epoch : UINT64_C(0)
            );
            break;
        }
        case 'W': {
            struct scheduler_core_event_token token;

            if (scanf(
                    " %" SCNu32 " %" SCNu64,
                    &event_index,
                    &event_epoch
                ) != 2) {
                return 2;
            }

            token.index = event_index;
            token.epoch = event_epoch;
            status = scheduler_core_block_current(
                &state,
                token,
                &previous,
                &next
            );
            printf("W %u", (unsigned int)status);

            if (status == SCHEDULER_CORE_OK) {
                print_identity(&previous);
                print_identity(&next);
            }

            putchar('\n');
            break;
        }
        case 'L': {
            unsigned int wake_all;
            size_t woken_count;
            uint64_t new_epoch;

            if (scanf(" %" SCNu32 " %u", &event_index, &wake_all) != 2 ||
                wake_all > 1U) {
                return 2;
            }

            status = scheduler_core_event_signal(
                &state,
                event_index,
                wake_all != 0U,
                &woken_count,
                &new_epoch
            );
            printf(
                "L %u %zu %" PRIu64 "\n",
                (unsigned int)status,
                status == SCHEDULER_CORE_OK ? woken_count : 0U,
                status == SCHEDULER_CORE_OK ? new_epoch : UINT64_C(0)
            );
            break;
        }
        case 'G':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2 ||
                index >= SCHEDULER_TASK_LIMIT) {
                return 2;
            }

            state.descriptors[index].generation = generation;
            status = scheduler_core_validate(&state);
            printf("G %u\n", (unsigned int)status);
            break;
        case 'U':
            if (scanf(
                    " %" SCNu32 " %" SCNu64,
                    &event_index,
                    &event_epoch
                ) != 2 || (size_t)event_index >= SCHEDULER_CORE_EVENT_LIMIT) {
                return 2;
            }

            state.event_epochs[event_index] = event_epoch;
            status = scheduler_core_validate(&state);
            printf("U %u\n", (unsigned int)status);
            break;
        case 'K': {
            unsigned int kind;

            if (scanf(" %u", &kind) != 1) {
                return 2;
            }

            if (kind == 0U && state.queue_count != 0U &&
                state.queue_head < SCHEDULER_CORE_QUEUE_CAPACITY) {
                size_t duplicate = state.queue_head + 1U;

                if (duplicate >= SCHEDULER_CORE_QUEUE_CAPACITY) {
                    duplicate = 0U;
                }

                state.ready_identities[duplicate] =
                    state.ready_identities[state.queue_head];
                state.ready_generations[duplicate] =
                    state.ready_generations[state.queue_head];
                state.queue_count = 2U;
            } else if (kind == 1U) {
                state.queue_head = SCHEDULER_CORE_QUEUE_CAPACITY;
            } else if (kind == 2U) {
                state.descriptors[0].state = SCHEDULER_TASK_RUNNING;
            } else if (kind == 3U) {
                state.queue_count = SCHEDULER_CORE_QUEUE_CAPACITY + 1U;
            } else if (kind == 4U) {
                state.descriptors[0].generation = 0U;
            } else if (kind == 5U && state.queue_count != 0U &&
                state.queue_head < SCHEDULER_CORE_QUEUE_CAPACITY) {
                state.ready_identities[state.queue_head] =
                    (uint8_t)(SCHEDULER_TASK_LIMIT + 1U);
                state.ready_generations[state.queue_head] = 1U;
            } else if (kind == 6U &&
                state.queue_head < SCHEDULER_CORE_QUEUE_CAPACITY) {
                state.ready_identities[state.queue_head] =
                    SCHEDULER_CORE_QUEUE_BOOTSTRAP;
                state.ready_generations[state.queue_head] = 0U;
            } else if (kind == 7U && state.queue_count != 0U &&
                state.queue_head < SCHEDULER_CORE_QUEUE_CAPACITY) {
                const uint8_t queued =
                    state.ready_identities[state.queue_head];

                if ((size_t)queued < SCHEDULER_TASK_LIMIT) {
                    state.descriptors[queued].queued = false;
                }
            } else if (kind == 8U) {
                state.current.bootstrap = false;
                state.current.index = 0U;
                state.current.generation = 2U;
            } else if (kind == 9U) {
                state.bootstrap_state = SCHEDULER_TASK_READY;
            } else if (kind == 10U) {
                state.descriptors[0].priority =
                    (uint8_t)(SCHEDULER_PRIORITY_MAX + 1U);
            } else if (kind == 11U) {
                state.bootstrap_wait_age =
                    (uint8_t)(SCHEDULER_CORE_MAXIMUM_WAIT_AGE + 1U);
            } else if (kind == 12U && !state.current.bootstrap) {
                state.descriptors[state.current.index].state =
                    SCHEDULER_TASK_BLOCKED;
            } else if (kind == 13U) {
                state.event_waiters[0] = UINT16_C(1);
            } else if (kind == 14U && state.queue_count != 0U &&
                state.queue_head < SCHEDULER_CORE_QUEUE_CAPACITY) {
                ++state.ready_generations[state.queue_head];
            } else if (kind == 15U && !state.current.bootstrap) {
                const uint16_t waiter_bit = (uint16_t)(
                    UINT16_C(1) << state.current.index
                );

                state.descriptors[state.current.index].state =
                    SCHEDULER_TASK_BLOCKED;
                state.descriptors[state.current.index].wait_event = 0U;
                state.descriptors[state.current.index].wait_epoch = 1U;
                state.event_waiters[0] = waiter_bit;
                state.event_epochs[0] = 2U;
                state.blocked_transitions = 1U;
            } else if (kind == 16U) {
                state.wakeups = 1U;
            } else if (kind == 17U) {
                state.reserved[0] = 1U;
            } else if (kind == 18U) {
                state.descriptors[0].state = SCHEDULER_TASK_SLEEPING;
                state.descriptors[0].wake_deadline = 0U;
                state.blocked_transitions = 1U;
            } else if (kind == 19U) {
                state.join_waiters[0] = UINT16_C(1);
            } else if (kind == 20U) {
                state.descriptors[0].state = SCHEDULER_TASK_JOIN_BLOCKED;
                state.descriptors[0].join_index = 0U;
                state.descriptors[0].join_generation = 1U;
                state.join_waiters[0] = UINT16_C(1);
                state.blocked_transitions = 1U;
            } else if (kind == 21U) {
                state.descriptors[0].join_completion_ready = true;
                state.descriptors[0].join_completion_index = 1U;
                state.descriptors[0].join_completion_generation = 1U;
            } else if (kind == 22U) {
                state.descriptors[0].cancel_requested = true;
            } else if (kind == 23U) {
                state.descriptors[0].join_completion_index = 1U;
            } else if (kind == 24U) {
                state.descriptors[0].reserved[0] = 1U;
            } else if (kind == 25U) {
                state.cancellations = 1U;
            }

            status = scheduler_core_validate(&state);
            printf("K %u\n", (unsigned int)status);
            break;
        }
        case 'S':
            print_snapshot();
            break;
        case 'V':
            status = scheduler_core_validate(&state);
            printf("V %u\n", (unsigned int)status);
            break;
        case 'N': {
            bool valid = true;
            bool lifecycle_flag;
            struct scheduler_core_event_token token;
            struct scheduler_task_handle completion;
            size_t woken_count;
            uint64_t new_epoch;

            handle.index = 0U;
            handle.generation = 1U;
            token.index = 0U;
            token.epoch = 1U;
            valid = valid && scheduler_core_initialize(NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_validate(NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_begin_create(NULL, &handle) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_begin_create(&state, NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_begin_create_with_priority(
                    &state,
                    SCHEDULER_PRIORITY_DEFAULT,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_resolve_handle(
                    NULL,
                    handle,
                    &task_state
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_resolve_handle(
                    &state,
                    handle,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_yield(NULL, &previous, &next) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_yield(&state, NULL, &next) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_exit_current(
                    &state,
                    &previous,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_event_observe(
                    &state,
                    0U,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_event_observe(
                    NULL,
                    0U,
                    &token
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_block_current(
                    &state,
                    token,
                    NULL,
                    &next
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_event_signal(
                    &state,
                    0U,
                    false,
                    NULL,
                    &new_epoch
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_event_signal(
                    &state,
                    0U,
                    false,
                    &woken_count,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_join_current(
                    &state,
                    handle,
                    NULL,
                    &previous,
                    &next
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_consume_join_completion(
                    &state,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_request_cancel(
                    &state,
                    handle,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_cancellation_point(
                    &state,
                    NULL,
                    &previous,
                    &next
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_sleep_current_until(
                    &state,
                    1U,
                    NULL,
                    &next
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_sleep_current_for(
                    &state,
                    1U,
                    &previous,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_advance_time(
                    &state,
                    1U,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_timer_tick(
                    &state,
                    NULL
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            lifecycle_flag = false;
            valid = valid && scheduler_core_request_cancel(
                    NULL,
                    handle,
                    &lifecycle_flag
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_consume_join_completion(
                    NULL,
                    &completion
                ) == SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && event_signal_boundary_checks();
            printf("N %u\n", valid ? 0U : (unsigned int)SCHEDULER_CORE_CORRUPTED);
            break;
        }
        default:
            return 2;
        }
    }

    return ferror(stdin) != 0 ? 3 : 0;
}

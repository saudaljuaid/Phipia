/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/scheduler_core.h"

static struct scheduler_core_state state;

static void print_identity(const struct scheduler_core_identity *identity)
{
    printf(
        " %u %" PRIu32 " %" PRIu64,
        identity->bootstrap ? 1U : 0U,
        identity->index,
        identity->generation
    );
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
        size_t position = state.queue_head + offset;

        if (position >= SCHEDULER_CORE_QUEUE_CAPACITY) {
            position -= SCHEDULER_CORE_QUEUE_CAPACITY;
        }

        print_identity(&state.ready_queue[position]);
    }

    printf(" | ");

    for (size_t index = 0U; index < SCHEDULER_TASK_LIMIT; ++index) {
        printf(
            "%u:%" PRIu64 ":%u%s",
            (unsigned int)state.descriptors[index].state,
            state.descriptors[index].generation,
            state.descriptors[index].queued ? 1U : 0U,
            index + 1U == SCHEDULER_TASK_LIMIT ? "" : ","
        );
    }

    printf(
        " | %zu %zu %zu %zu %zu\n",
        state.successful_creations,
        state.failed_creations,
        state.context_switches,
        state.completed_tasks,
        state.reaped_tasks
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
        case 'G':
            if (scanf(" %" SCNu32 " %" SCNu64, &index, &generation) != 2 ||
                index >= SCHEDULER_TASK_LIMIT) {
                return 2;
            }

            state.descriptors[index].generation = generation;
            status = scheduler_core_validate(&state);
            printf("G %u\n", (unsigned int)status);
            break;
        case 'K': {
            unsigned int kind;

            if (scanf(" %u", &kind) != 1) {
                return 2;
            }

            if (kind == 0U && state.queue_count != 0U) {
                size_t duplicate = state.queue_head + 1U;

                if (duplicate >= SCHEDULER_CORE_QUEUE_CAPACITY) {
                    duplicate = 0U;
                }

                state.ready_queue[duplicate] =
                    state.ready_queue[state.queue_head];
                state.queue_count = 2U;
            } else if (kind == 1U) {
                state.queue_head = SCHEDULER_CORE_QUEUE_CAPACITY;
            } else if (kind == 2U) {
                state.descriptors[0].state = SCHEDULER_TASK_RUNNING;
            } else if (kind == 3U) {
                state.queue_count = SCHEDULER_CORE_QUEUE_CAPACITY + 1U;
            } else if (kind == 4U) {
                state.descriptors[0].generation = 0U;
            } else if (kind == 5U && state.queue_count != 0U) {
                state.ready_queue[state.queue_head].bootstrap = false;
                state.ready_queue[state.queue_head].index =
                    (uint32_t)SCHEDULER_TASK_LIMIT;
                state.ready_queue[state.queue_head].generation = 1U;
            } else if (kind == 6U) {
                state.ready_queue[state.queue_head].bootstrap = true;
                state.ready_queue[state.queue_head].index =
                    SCHEDULER_INVALID_INDEX;
                state.ready_queue[state.queue_head].generation = 0U;
            } else if (kind == 7U && state.queue_count != 0U) {
                state.descriptors[
                    state.ready_queue[state.queue_head].index
                ].queued = false;
            } else if (kind == 8U) {
                state.current.bootstrap = false;
                state.current.index = 0U;
                state.current.generation = 2U;
            } else if (kind == 9U) {
                state.bootstrap_state = SCHEDULER_TASK_READY;
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

            handle.index = 0U;
            handle.generation = 1U;
            valid = valid && scheduler_core_initialize(NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_validate(NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_begin_create(NULL, &handle) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
            valid = valid && scheduler_core_begin_create(&state, NULL) ==
                SCHEDULER_CORE_NULL_ARGUMENT;
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
            printf("N %u\n", valid ? 0U : (unsigned int)SCHEDULER_CORE_CORRUPTED);
            break;
        }
        default:
            return 2;
        }
    }

    return ferror(stdin) != 0 ? 3 : 0;
}

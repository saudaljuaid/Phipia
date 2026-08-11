/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/preempt_core.h"

static void print_boolean_status(
    char operation,
    enum preempt_status status,
    bool value
)
{
    printf("%c %d %u\n", operation, (int)status, value ? 1U : 0U);
}

static void snapshot(const struct preempt_core_state *state)
{
    enum preempt_status status = preempt_core_validate(state);

    printf(
        "S %d %u %u %u",
        (int)status,
        (unsigned int)state->initialized,
        (unsigned int)state->poisoned,
        state->online_count
    );
    for (uint32_t index = 0U; index < PREEMPT_CPU_LIMIT; ++index) {
        const struct preempt_core_cpu *cpu = &state->cpus[index];

        printf(
            " %u:%u:%u:%u",
            cpu->irq_depth,
            cpu->preempt_count,
            (unsigned int)cpu->need_resched,
            (unsigned int)cpu->online
        );
    }
    putchar('\n');
}

static void corrupt_and_probe(struct preempt_core_state *state, uint32_t kind)
{
    enum preempt_status status;

    switch (kind) {
    case 0U:
        ++state->online_count;
        break;
    case 1U:
        state->cpus[1].irq_depth = 1U;
        break;
    case 2U:
        state->cpus[1].preempt_count = 1U;
        break;
    case 3U:
        state->cpus[1].need_resched = 1U;
        break;
    case 4U:
        state->cpus[0].irq_depth = PREEMPT_NESTING_LIMIT + 1U;
        break;
    case 5U:
        state->cpus[0].preempt_count = PREEMPT_NESTING_LIMIT + 1U;
        break;
    case 6U:
        state->cpus[0].online = 2U;
        break;
    case 7U:
        state->cpus[0].need_resched = 2U;
        break;
    case 8U:
        state->reserved[0] = 1U;
        break;
    case 9U:
        state->cpus[0].online = 0U;
        state->online_count = 0U;
        break;
    case 10U:
        state->initialized = 2U;
        break;
    case 11U:
        state->poisoned = 2U;
        break;
    case 12U:
        state->cpus[0].reserved[0] = 1U;
        break;
    default:
        printf("K %d %u\n", (int)PREEMPT_STATUS_INVALID_CPU, 0U);
        return;
    }

    status = preempt_core_request_reschedule(state, 0U);
    printf("K %d %u\n", (int)status, (unsigned int)state->poisoned);
}

static bool null_argument_checks(struct preempt_core_state *state)
{
    bool eligible = true;
    struct preempt_cpu_snapshot snapshot;

    preempt_core_clear(NULL);
    return preempt_core_initialize(NULL, 0U) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_validate(NULL) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_cpu_online(NULL, 0U) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_cpu_offline(NULL, 0U) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_irq_enter(NULL, 0U) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_irq_exit(NULL, 0U, &eligible) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_irq_exit(state, 0U, NULL) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_disable(NULL, 0U) == PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_enable(NULL, 0U, &eligible) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_enable(state, 0U, NULL) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_request_reschedule(NULL, 0U) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_cancel_reschedule(NULL, 0U) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_reschedule_eligible(NULL, 0U, &eligible) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_reschedule_eligible(state, 0U, NULL) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_take_reschedule(NULL, 0U) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_cpu_snapshot(NULL, 0U, &snapshot) ==
            PREEMPT_STATUS_NULL_ARGUMENT &&
        preempt_core_cpu_snapshot(state, 0U, NULL) ==
            PREEMPT_STATUS_NULL_ARGUMENT;
}

int main(void)
{
    struct preempt_core_state state;
    char operation;

    preempt_core_clear(&state);

    while (scanf(" %c", &operation) == 1) {
        uint32_t cpu_index;
        enum preempt_status status;
        bool eligible = false;

        switch (operation) {
        case 'Z':
            preempt_core_clear(&state);
            puts("Z");
            break;
        case 'I':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf("I %d\n", (int)preempt_core_initialize(&state, cpu_index));
            break;
        case 'V':
            printf("V %d\n", (int)preempt_core_validate(&state));
            break;
        case 'O':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf("O %d\n", (int)preempt_core_cpu_online(&state, cpu_index));
            break;
        case 'F':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf("F %d\n", (int)preempt_core_cpu_offline(&state, cpu_index));
            break;
        case 'N':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf("N %d\n", (int)preempt_core_irq_enter(&state, cpu_index));
            break;
        case 'X':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            status = preempt_core_irq_exit(&state, cpu_index, &eligible);
            print_boolean_status('X', status, eligible);
            break;
        case 'D':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf("D %d\n", (int)preempt_core_disable(&state, cpu_index));
            break;
        case 'E':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            status = preempt_core_enable(&state, cpu_index, &eligible);
            print_boolean_status('E', status, eligible);
            break;
        case 'R':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf(
                "R %d\n",
                (int)preempt_core_request_reschedule(&state, cpu_index)
            );
            break;
        case 'C':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf(
                "C %d\n",
                (int)preempt_core_cancel_reschedule(&state, cpu_index)
            );
            break;
        case 'G':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            status = preempt_core_reschedule_eligible(
                &state,
                cpu_index,
                &eligible
            );
            print_boolean_status('G', status, eligible);
            break;
        case 'Q': {
            struct preempt_cpu_snapshot cpu_snapshot;

            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            status = preempt_core_cpu_snapshot(
                &state,
                cpu_index,
                &cpu_snapshot
            );
            printf(
                "Q %d %u %u %u %u\n",
                (int)status,
                cpu_snapshot.irq_depth,
                cpu_snapshot.preempt_count,
                cpu_snapshot.need_resched ? 1U : 0U,
                cpu_snapshot.online ? 1U : 0U
            );
            break;
        }
        case 'T':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            printf(
                "T %d\n",
                (int)preempt_core_take_reschedule(&state, cpu_index)
            );
            break;
        case 'A':
            if (scanf(" %u", &cpu_index) != 1 ||
                cpu_index >= PREEMPT_CPU_LIMIT) {
                return 2;
            }
            state.cpus[cpu_index].irq_depth = PREEMPT_NESTING_LIMIT;
            puts("A");
            break;
        case 'B':
            if (scanf(" %u", &cpu_index) != 1 ||
                cpu_index >= PREEMPT_CPU_LIMIT) {
                return 2;
            }
            state.cpus[cpu_index].preempt_count = PREEMPT_NESTING_LIMIT;
            puts("B");
            break;
        case 'K':
            if (scanf(" %u", &cpu_index) != 1) {
                return 2;
            }
            corrupt_and_probe(&state, cpu_index);
            break;
        case 'U':
            printf("U %u\n", null_argument_checks(&state) ? 1U : 0U);
            break;
        case 'S':
            snapshot(&state);
            break;
        default:
            return 2;
        }
    }

    return ferror(stdin) != 0 || ferror(stdout) != 0 ? 1 : 0;
}

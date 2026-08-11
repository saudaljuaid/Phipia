/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/xstate_core.h"

static void default_layout(struct xstate_layout *layout)
{
    layout->enabled_mask = XSTATE_BOUNDED_MASK;
    layout->bounded_hardware_mask = XSTATE_BOUNDED_MASK;
    layout->area_size = UINT32_C(832);
    layout->alignment = XSTATE_AREA_ALIGNMENT;
    layout->avx_component_size = UINT32_C(256);
    layout->avx_component_offset = XSTATE_MINIMUM_AREA_SIZE;
}

static void print_status(char operation, enum xstate_status status)
{
    printf("%c %d\n", operation, (int)status);
}

static bool null_checks(
    struct xstate_core_state *state,
    const struct xstate_layout *layout
)
{
    struct xstate_layout derived;
    struct xstate_layout_probe probe = {
        .cpuid_xcr0_mask = XSTATE_BOUNDED_MASK,
        .xcr0 = XSTATE_BOUNDED_MASK,
        .enabled_area_size = UINT32_C(832),
        .maximum_area_size = UINT32_C(832),
        .avx_component_size = UINT32_C(256),
        .avx_component_offset = XSTATE_MINIMUM_AREA_SIZE,
        .xsave = UINT8_C(1),
        .osxsave = UINT8_C(1),
        .leaf_d = UINT8_C(1),
        .reserved = 0U
    };
    struct xstate_task_handle handle = {0U, 1U};
    struct xstate_task_snapshot task_snapshot = {0};
    struct xstate_cpu_snapshot cpu_snapshot = {0};

    xstate_core_clear(NULL);
    return xstate_layout_derive(NULL, &derived) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_layout_derive(&probe, NULL) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_layout_validate(NULL) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_area_validate(NULL, UINT32_C(64), UINT32_C(832)) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_initialize(NULL, layout, 0U) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_initialize(state, NULL, 0U) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_validate(NULL) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_cpu_online(NULL, 0U) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_cpu_offline(NULL, 0U) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_create(NULL, &handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_create(state, NULL) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_mark_image_ready(NULL, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_publish(NULL, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_abort(NULL, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_record_save(NULL, 0U, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_record_restore(NULL, 0U, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_begin_destroy(NULL, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_abort_destroy(NULL, handle) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_reap(NULL, handle) == XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_require_features(NULL, 0U) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_unexpected_nm(NULL, 0U) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_snapshot(NULL, handle, &task_snapshot) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_task_snapshot(state, handle, NULL) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_cpu_snapshot(NULL, 0U, &cpu_snapshot) ==
            XSTATE_STATUS_NULL_ARGUMENT &&
        xstate_core_cpu_snapshot(state, 0U, NULL) ==
            XSTATE_STATUS_NULL_ARGUMENT;
}

static bool corruption_checks(const struct xstate_layout *layout)
{
    struct xstate_core_state state;
    struct xstate_task_handle handle = {0U, 0U};
    enum xstate_status status;

    for (uint32_t kind = 0U; kind < UINT32_C(16); ++kind) {
        xstate_core_clear(&state);
        if (xstate_core_initialize(&state, layout, 0U) != XSTATE_STATUS_OK ||
            xstate_core_task_create(&state, &handle) != XSTATE_STATUS_OK ||
            xstate_core_task_mark_image_ready(&state, handle) !=
                XSTATE_STATUS_OK ||
            xstate_core_task_publish(&state, handle) != XSTATE_STATUS_OK) {
            return false;
        }

        switch (kind) {
        case 0U:
            ++state.active_tasks;
            break;
        case 1U:
            state.cpus[0].online = 2U;
            break;
        case 2U:
            state.cpus[0].reserved[0] = 1U;
            break;
        case 3U:
            state.tasks[handle.index].state = UINT8_C(9);
            break;
        case 4U:
            state.tasks[handle.index].image_ready = 2U;
            break;
        case 5U:
            state.tasks[handle.index].reserved[0] = 1U;
            break;
        case 6U:
            state.tasks[handle.index].generation = 0U;
            break;
        case 7U:
            state.tasks[handle.index].owner_cpu = 0U;
            break;
        case 8U:
            state.tasks[handle.index].save_count = 1U;
            break;
        case 9U:
            state.cpus[0].loaded_task = handle.index;
            break;
        case 10U:
            state.cpus[0].loaded_generation = handle.generation;
            break;
        case 11U:
            ++state.online_cpus;
            break;
        case 12U:
            state.layout.area_size = UINT32_C(1);
            break;
        case 13U:
            state.reserved[0] = 1U;
            break;
        case 14U:
            state.initialized = 2U;
            break;
        default:
            state.unexpected_nm_count = 1U;
            break;
        }
        status = xstate_core_require_features(&state, 0U);
        if (status != XSTATE_STATUS_CORRUPTED ||
            state.poisoned != UINT8_C(1)) {
            return false;
        }
    }
    return true;
}

static bool boundary_checks(const struct xstate_layout *layout)
{
    struct xstate_core_state state;
    struct xstate_task_handle handles[XSTATE_TASK_LIMIT];
    enum xstate_status status;

    xstate_core_clear(&state);
    if (xstate_core_initialize(&state, layout, 0U) != XSTATE_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        if (xstate_core_task_create(&state, &handles[index]) !=
            XSTATE_STATUS_OK) {
            return false;
        }
    }
    if (xstate_core_task_create(&state, &handles[0]) !=
        XSTATE_STATUS_NO_TASK_SLOT) {
        return false;
    }

    xstate_core_clear(&state);
    if (xstate_core_initialize(&state, layout, 0U) != XSTATE_STATUS_OK) {
        return false;
    }
    state.tasks[0].generation = UINT32_MAX - 1U;
    if (xstate_core_task_create(&state, &handles[0]) != XSTATE_STATUS_OK ||
        handles[0].generation != UINT32_MAX ||
        xstate_core_task_abort(&state, handles[0]) != XSTATE_STATUS_OK ||
        state.tasks[0].state != (uint8_t)XSTATE_TASK_RETIRED) {
        return false;
    }

    if (xstate_core_task_create(&state, &handles[0]) != XSTATE_STATUS_OK ||
        xstate_core_task_mark_image_ready(&state, handles[0]) !=
            XSTATE_STATUS_OK ||
        xstate_core_task_publish(&state, handles[0]) != XSTATE_STATUS_OK ||
        xstate_core_record_restore(&state, 0U, handles[0]) !=
            XSTATE_STATUS_OK) {
        return false;
    }
    state.tasks[handles[0].index].save_count = UINT64_MAX - 1U;
    state.tasks[handles[0].index].restore_count = UINT64_MAX;
    status = xstate_core_record_save(&state, 0U, handles[0]);
    if (status != XSTATE_STATUS_OK ||
        xstate_core_record_restore(&state, 0U, handles[0]) !=
            XSTATE_STATUS_COUNTER_EXHAUSTED ||
        state.poisoned != UINT8_C(1)) {
        return false;
    }
    return true;
}

int main(void)
{
    struct xstate_core_state state;
    struct xstate_layout layout;
    char operation;

    xstate_core_clear(&state);
    default_layout(&layout);

    while (scanf(" %c", &operation) == 1) {
        uint32_t first;
        uint32_t second;
        struct xstate_task_handle handle = {0U, 0U};
        enum xstate_status status;

        switch (operation) {
        case 'W':
            xstate_core_clear(&state);
            print_status(operation, XSTATE_STATUS_OK);
            break;
        case 'L': {
            struct xstate_layout_probe probe;
            unsigned int xsave;
            unsigned int osxsave;
            unsigned int leaf_d;
            unsigned int reserved;

            if (scanf(
                    " %u %u %u %u %" SCNu64 " %" SCNu64
                    " %u %u %u %u",
                    &xsave, &osxsave, &leaf_d, &reserved,
                    &probe.cpuid_xcr0_mask, &probe.xcr0,
                    &probe.enabled_area_size, &probe.maximum_area_size,
                    &probe.avx_component_size,
                    &probe.avx_component_offset
                ) != 10) {
                return 2;
            }
            probe.xsave = (uint8_t)xsave;
            probe.osxsave = (uint8_t)osxsave;
            probe.leaf_d = (uint8_t)leaf_d;
            probe.reserved = (uint8_t)reserved;
            status = xstate_layout_derive(&probe, &layout);
            printf(
                "L %d %" PRIu64 " %" PRIu64 " %u %u %u %u\n",
                (int)status, layout.enabled_mask,
                layout.bounded_hardware_mask, layout.area_size,
                layout.alignment, layout.avx_component_size,
                layout.avx_component_offset
            );
            break;
        }
        case 'Y': {
            uintptr_t address;
            size_t bytes;

            if (scanf(" %" SCNuPTR " %zu", &address, &bytes) != 2) {
                return 2;
            }
            print_status(operation,
                xstate_area_validate(&layout, address, bytes));
            break;
        }
        case 'I':
            if (scanf(" %u", &first) != 1) {
                return 2;
            }
            print_status(operation,
                xstate_core_initialize(&state, &layout, first));
            break;
        case 'O':
        case 'F':
            if (scanf(" %u", &first) != 1) {
                return 2;
            }
            status = operation == 'O' ?
                xstate_core_cpu_online(&state, first) :
                xstate_core_cpu_offline(&state, first);
            print_status(operation, status);
            break;
        case 'C':
            status = xstate_core_task_create(&state, &handle);
            printf("C %d %u %u\n", (int)status,
                handle.index, handle.generation);
            break;
        case 'M':
        case 'B':
        case 'A':
        case 'D':
        case 'U':
        case 'X':
            if (scanf(" %u %u", &handle.index, &handle.generation) != 2) {
                return 2;
            }
            if (operation == 'M') {
                status = xstate_core_task_mark_image_ready(&state, handle);
            } else if (operation == 'B') {
                status = xstate_core_task_publish(&state, handle);
            } else if (operation == 'A') {
                status = xstate_core_task_abort(&state, handle);
            } else if (operation == 'D') {
                status = xstate_core_task_begin_destroy(&state, handle);
            } else if (operation == 'U') {
                status = xstate_core_task_abort_destroy(&state, handle);
            } else {
                status = xstate_core_task_reap(&state, handle);
            }
            print_status(operation, status);
            break;
        case 'R':
        case 'S':
            if (scanf(" %u %u %u", &first, &handle.index,
                    &handle.generation) != 3) {
                return 2;
            }
            status = operation == 'R' ?
                xstate_core_record_restore(&state, first, handle) :
                xstate_core_record_save(&state, first, handle);
            print_status(operation, status);
            break;
        case 'E': {
            uint64_t mask;

            if (scanf(" %" SCNu64, &mask) != 1) {
                return 2;
            }
            print_status(operation,
                xstate_core_require_features(&state, mask));
            break;
        }
        case 'N':
            if (scanf(" %u", &first) != 1) {
                return 2;
            }
            print_status(operation,
                xstate_core_unexpected_nm(&state, first));
            break;
        case 'Q': {
            struct xstate_task_snapshot snapshot = {0};

            if (scanf(" %u %u", &handle.index, &handle.generation) != 2) {
                return 2;
            }
            status = xstate_core_task_snapshot(&state, handle, &snapshot);
            printf("Q %d %d %u %u %" PRIu64 " %" PRIu64 " %u\n",
                (int)status, (int)snapshot.state, snapshot.generation,
                snapshot.owner_cpu, snapshot.save_count,
                snapshot.restore_count, snapshot.image_ready ? 1U : 0U);
            break;
        }
        case 'G': {
            struct xstate_cpu_snapshot snapshot = {0};

            if (scanf(" %u", &first) != 1) {
                return 2;
            }
            status = xstate_core_cpu_snapshot(&state, first, &snapshot);
            printf("G %d %u %u %u\n", (int)status,
                snapshot.loaded_task, snapshot.loaded_generation,
                snapshot.online ? 1U : 0U);
            break;
        }
        case 'J':
            if (scanf(" %u %u", &first, &second) != 2 ||
                first >= XSTATE_TASK_LIMIT) {
                return 2;
            }
            state.tasks[first].generation = second;
            print_status(operation, xstate_core_validate(&state));
            break;
        case 'V':
            printf("V %d %u %u %u %u %u\n",
                (int)xstate_core_validate(&state), state.active_tasks,
                state.retired_tasks, state.online_cpus,
                state.unexpected_nm_count, (unsigned int)state.poisoned);
            break;
        case 'H':
            printf("H %u\n", (null_checks(&state, &layout) &&
                corruption_checks(&layout) && boundary_checks(&layout)) ?
                1U : 0U);
            break;
        default:
            return 3;
        }
    }
    return 0;
}

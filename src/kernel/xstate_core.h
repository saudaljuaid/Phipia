/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_XSTATE_CORE_H
#define ZENITH_XSTATE_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <zenith/xstate.h>

struct xstate_core_task {
    uint64_t save_count;
    uint64_t restore_count;
    uint32_t generation;
    uint32_t owner_cpu;
    uint8_t state;
    uint8_t image_ready;
    uint8_t reserved[6];
};

struct xstate_core_cpu {
    uint32_t loaded_task;
    uint32_t loaded_generation;
    uint8_t online;
    uint8_t reserved[7];
};

struct xstate_core_state {
    struct xstate_layout layout;
    struct xstate_core_task tasks[XSTATE_TASK_LIMIT];
    struct xstate_core_cpu cpus[XSTATE_CPU_LIMIT];
    uint32_t active_tasks;
    uint32_t retired_tasks;
    uint32_t online_cpus;
    uint32_t unexpected_nm_count;
    uint8_t initialized;
    uint8_t poisoned;
    uint8_t reserved[6];
};

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void xstate_core_clear(struct xstate_core_state *state);
enum xstate_status xstate_layout_derive(
    const struct xstate_layout_probe *probe,
    struct xstate_layout *layout
);
enum xstate_status xstate_layout_validate(
    const struct xstate_layout *layout
);
enum xstate_status xstate_area_validate(
    const struct xstate_layout *layout,
    uintptr_t address,
    size_t byte_count
);
enum xstate_status xstate_core_initialize(
    struct xstate_core_state *state,
    const struct xstate_layout *layout,
    uint32_t boot_cpu
);
enum xstate_status xstate_core_validate(
    const struct xstate_core_state *state
);
enum xstate_status xstate_core_cpu_online(
    struct xstate_core_state *state,
    uint32_t cpu_index
);
enum xstate_status xstate_core_cpu_offline(
    struct xstate_core_state *state,
    uint32_t cpu_index
);
enum xstate_status xstate_core_task_create(
    struct xstate_core_state *state,
    struct xstate_task_handle *handle
);
enum xstate_status xstate_core_task_mark_image_ready(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_task_publish(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_task_abort(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
/* Call these only after the corresponding eager XSAVE/XRSTOR instruction has
 * completed. The core records ownership; it does not access the image. */
enum xstate_status xstate_core_record_save(
    struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_record_restore(
    struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_task_begin_destroy(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_task_abort_destroy(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
enum xstate_status xstate_core_task_reap(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
);
/* A feature request outside the enabled XCR0 mask and every #NM on an online
 * CPU are fatal eager-switching invariant failures and poison the core. */
enum xstate_status xstate_core_require_features(
    struct xstate_core_state *state,
    uint64_t required_mask
);
enum xstate_status xstate_core_unexpected_nm(
    struct xstate_core_state *state,
    uint32_t cpu_index
);
enum xstate_status xstate_core_task_snapshot(
    const struct xstate_core_state *state,
    struct xstate_task_handle handle,
    struct xstate_task_snapshot *snapshot
);
enum xstate_status xstate_core_cpu_snapshot(
    const struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_cpu_snapshot *snapshot
);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_XSTATE_RUNTIME_H
#define ZENITH_XSTATE_RUNTIME_H

#include <stdbool.h>

#include <zenith/xstate.h>

/* Scheduler-private mutations.  Creation/destruction are called with the
 * scheduler preemption guard held.  Switching is called only with IF clear at
 * the final interrupt-return handoff; NMI/IST code must remain xstate-free. */
enum xstate_status xstate_runtime_initialize(void);
enum xstate_status xstate_runtime_task_create(
    struct xstate_task_handle *handle
);
enum xstate_status xstate_runtime_task_destroy(
    struct xstate_task_handle handle
);
enum xstate_status xstate_runtime_switch(
    bool previous_bootstrap,
    struct xstate_task_handle previous,
    bool next_bootstrap,
    struct xstate_task_handle next
);
enum xstate_status xstate_runtime_task_validate(
    struct xstate_task_handle handle,
    bool expected_loaded
);

#endif

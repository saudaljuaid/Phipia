/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_PROCESS_CORE_H
#define ZENITH_PROCESS_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/process.h>

struct process_core_slot {
    process_handle_t parent;
    address_space_handle_t address_space;
    uint64_t completion_epoch;
    uint32_t generation;
    int32_t exit_status;
    uint16_t task_attachment_count;
    uint16_t child_count;
    uint8_t state;
    uint8_t reserved[7];
};

struct process_core_state {
    struct process_core_slot slots[PROCESS_LIMIT];
    uint16_t occupied_count;
    uint8_t initialized;
    uint8_t poisoned;
    uint32_t reserved;
};

/* Allocation-free and lock-free: the runtime owner serializes all calls.
 * Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void process_core_clear(struct process_core_state *core);
enum process_status process_core_initialize(struct process_core_state *core);
enum process_status process_core_validate(const struct process_core_state *core);
enum process_status process_core_create(
    struct process_core_state *core,
    process_handle_t parent,
    address_space_handle_t address_space,
    process_handle_t *process
);
enum process_status process_core_attach_task(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_detach_task(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_block(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_make_runnable(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_exit(
    struct process_core_state *core,
    process_handle_t process,
    int32_t exit_status
);
enum process_status process_core_begin_reap(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_reap(
    struct process_core_state *core,
    process_handle_t process
);
enum process_status process_core_wait_observe(
    const struct process_core_state *core,
    process_handle_t waiter,
    process_handle_t child,
    struct process_wait_token *token,
    int32_t *exit_status
);
enum process_status process_core_wait_complete(
    const struct process_core_state *core,
    process_handle_t waiter,
    struct process_wait_token token,
    int32_t *exit_status
);
enum process_status process_core_snapshot(
    const struct process_core_state *core,
    process_handle_t process,
    struct process_snapshot *snapshot
);
enum process_frame_status process_core_validate_initial_frame(
    const struct process_user_frame *frame
);
enum process_frame_status process_core_validate_return_frame(
    const struct process_user_frame *frame
);
enum process_fault_disposition process_core_fault_disposition(uint64_t cs);

/* Internal regression probe used by the standalone host runner. */
bool process_core_resolution_output_self_test(void);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "process_core.h"

#define PROCESS_HANDLE_SLOT_MASK UINT64_C(0xFFFFFFFF)

_Static_assert(PROCESS_LIMIT == 16U,
    "process core is deliberately bounded to sixteen slots");
_Static_assert(sizeof(struct process_core_slot) == 48U,
    "process slot layout changed");

static bool address_space_handle_valid(address_space_handle_t handle)
{
    const uint64_t encoded_slot = handle & PROCESS_HANDLE_SLOT_MASK;
    const uint64_t generation = handle >> 32U;

    return encoded_slot >= 1U && encoded_slot <= ADDRESS_SPACE_LIMIT &&
        generation != 0U;
}

static process_handle_t make_handle(uint32_t slot, uint32_t generation)
{
    return ((uint64_t)generation << 32U) | ((uint64_t)slot + 1U);
}

static void slot_payload_clear(struct process_core_slot *slot)
{
    slot->parent = 0U;
    slot->address_space = 0U;
    slot->completion_epoch = 0U;
    slot->exit_status = 0;
    slot->task_attachment_count = 0U;
    slot->child_count = 0U;
    slot->state = PROCESS_STATE_FREE;
    for (size_t index = 0U; index < sizeof(slot->reserved); ++index) {
        slot->reserved[index] = 0U;
    }
}

static bool slot_reserved_clear(const struct process_core_slot *slot)
{
    for (size_t index = 0U; index < sizeof(slot->reserved); ++index) {
        if (slot->reserved[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool free_slot_valid(const struct process_core_slot *slot)
{
    return slot->parent == 0U && slot->address_space == 0U &&
        slot->completion_epoch == 0U && slot->exit_status == 0 &&
        slot->task_attachment_count == 0U && slot->child_count == 0U &&
        slot_reserved_clear(slot);
}

static enum process_status core_gate(const struct process_core_state *core)
{
    if (core == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    if (core->poisoned != 0U) {
        return PROCESS_STATUS_POISONED;
    }
    if (core->initialized == 0U) {
        return PROCESS_STATUS_NOT_INITIALIZED;
    }
    return PROCESS_STATUS_OK;
}

static enum process_status resolve_const(
    const struct process_core_state *core,
    process_handle_t handle,
    const struct process_core_slot **slot,
    uint32_t *slot_index
)
{
    const uint64_t encoded_slot = handle & PROCESS_HANDLE_SLOT_MASK;
    const uint64_t generation = handle >> 32U;
    enum process_status status;
    uint32_t index;

    if (slot == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    *slot = NULL;
    status = core_gate(core);
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (encoded_slot == 0U || encoded_slot > PROCESS_LIMIT ||
        generation == 0U) {
        return PROCESS_STATUS_INVALID_HANDLE;
    }

    index = (uint32_t)(encoded_slot - 1U);
    if (core->slots[index].state == PROCESS_STATE_FREE ||
        core->slots[index].generation != (uint32_t)generation) {
        return PROCESS_STATUS_STALE_HANDLE;
    }

    *slot = &core->slots[index];
    if (slot_index != NULL) {
        *slot_index = index;
    }
    return PROCESS_STATUS_OK;
}

static enum process_status resolve_mutable(
    struct process_core_state *core,
    process_handle_t handle,
    struct process_core_slot **slot,
    uint32_t *slot_index
)
{
    const struct process_core_slot *resolved;
    uint32_t resolved_index;
    enum process_status status;

    if (slot == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }

    status = resolve_const(
        core,
        handle,
        &resolved,
        &resolved_index
    );
    if (status != PROCESS_STATUS_OK) {
        return status;
    }

    /* The mutable entry point owns serialization; reuse the exact slot that
     * the const resolver authenticated instead of decoding the handle again. */
    *slot = (struct process_core_slot *)(uintptr_t)resolved;
    if (slot_index != NULL) {
        *slot_index = resolved_index;
    }
    return PROCESS_STATUS_OK;
}

void process_core_clear(struct process_core_state *core)
{
    if (core == NULL) {
        return;
    }

    for (size_t index = 0U; index < PROCESS_LIMIT; ++index) {
        core->slots[index].generation = 0U;
        slot_payload_clear(&core->slots[index]);
    }
    core->occupied_count = 0U;
    core->initialized = 0U;
    core->poisoned = 0U;
    core->reserved = 0U;
}

enum process_status process_core_validate(const struct process_core_state *core)
{
    uint16_t occupied = 0U;

    if (core == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    if (core->poisoned > UINT8_C(1)) {
        return PROCESS_STATUS_CORRUPTED;
    }
    if (core->poisoned != 0U) {
        return PROCESS_STATUS_POISONED;
    }
    if (core->initialized == 0U) {
        return PROCESS_STATUS_NOT_INITIALIZED;
    }
    if (core->initialized != UINT8_C(1) || core->reserved != 0U ||
        core->occupied_count > PROCESS_LIMIT) {
        return PROCESS_STATUS_CORRUPTED;
    }

    for (uint32_t index = 0U; index < PROCESS_LIMIT; ++index) {
        const struct process_core_slot *slot = &core->slots[index];

        if (slot->state == PROCESS_STATE_FREE) {
            if (!free_slot_valid(slot)) {
                return PROCESS_STATUS_CORRUPTED;
            }
            continue;
        }
        if (slot->state != PROCESS_STATE_RUNNABLE &&
            slot->state != PROCESS_STATE_BLOCKED &&
            slot->state != PROCESS_STATE_EXITED &&
            slot->state != PROCESS_STATE_DYING) {
            return PROCESS_STATUS_CORRUPTED;
        }
        if (slot->generation == 0U || slot->completion_epoch == 0U ||
            !address_space_handle_valid(slot->address_space) ||
            !slot_reserved_clear(slot)) {
            return PROCESS_STATUS_CORRUPTED;
        }
        if ((slot->state == PROCESS_STATE_RUNNABLE ||
             slot->state == PROCESS_STATE_BLOCKED) &&
            slot->exit_status != 0) {
            return PROCESS_STATUS_CORRUPTED;
        }
        if (slot->state == PROCESS_STATE_BLOCKED &&
            slot->task_attachment_count == 0U) {
            return PROCESS_STATUS_CORRUPTED;
        }
        if (slot->state == PROCESS_STATE_DYING &&
            (slot->task_attachment_count != 0U || slot->child_count != 0U)) {
            return PROCESS_STATUS_CORRUPTED;
        }
        ++occupied;

        if (slot->parent != 0U) {
            const struct process_core_slot *ancestor;
            process_handle_t cursor = slot->parent;
            uint32_t depth = 0U;

            while (cursor != 0U) {
                const uint64_t parent_slot =
                    cursor & PROCESS_HANDLE_SLOT_MASK;
                const uint64_t parent_generation = cursor >> 32U;

                if (parent_slot == 0U || parent_slot > PROCESS_LIMIT ||
                    parent_generation == 0U) {
                    return PROCESS_STATUS_CORRUPTED;
                }
                ancestor = &core->slots[parent_slot - 1U];
                if (ancestor->state == PROCESS_STATE_FREE ||
                    ancestor->generation != (uint32_t)parent_generation ||
                    cursor == make_handle(index, slot->generation) ||
                    ++depth > PROCESS_LIMIT) {
                    return PROCESS_STATUS_CORRUPTED;
                }
                cursor = ancestor->parent;
            }
        }
    }

    if (occupied != core->occupied_count) {
        return PROCESS_STATUS_CORRUPTED;
    }

    for (uint32_t parent_index = 0U;
         parent_index < PROCESS_LIMIT;
         ++parent_index) {
        const struct process_core_slot *parent = &core->slots[parent_index];
        const process_handle_t parent_handle = make_handle(
            parent_index,
            parent->generation
        );
        uint16_t children = 0U;

        if (parent->state == PROCESS_STATE_FREE) {
            continue;
        }
        for (uint32_t child_index = 0U;
             child_index < PROCESS_LIMIT;
             ++child_index) {
            if (core->slots[child_index].state != PROCESS_STATE_FREE &&
                core->slots[child_index].parent == parent_handle) {
                ++children;
            }
        }
        if (children != parent->child_count) {
            return PROCESS_STATUS_CORRUPTED;
        }
    }
    return PROCESS_STATUS_OK;
}

enum process_status process_core_initialize(struct process_core_state *core)
{
    if (core == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    if (core->initialized != 0U) {
        return PROCESS_STATUS_ALREADY_INITIALIZED;
    }
    process_core_clear(core);
    core->initialized = UINT8_C(1);
    return PROCESS_STATUS_OK;
}

enum process_status process_core_create(
    struct process_core_state *core,
    process_handle_t parent,
    address_space_handle_t address_space,
    process_handle_t *process
)
{
    struct process_core_slot *parent_slot = NULL;
    enum process_status status;
    bool exhausted = false;

    if (process == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    *process = 0U;
    status = core_gate(core);
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (!address_space_handle_valid(address_space)) {
        return PROCESS_STATUS_INVALID_ADDRESS_SPACE;
    }
    if (parent != 0U) {
        status = resolve_mutable(core, parent, &parent_slot, NULL);
        if (status != PROCESS_STATUS_OK ||
            (parent_slot->state != PROCESS_STATE_RUNNABLE &&
             parent_slot->state != PROCESS_STATE_BLOCKED)) {
            return PROCESS_STATUS_INVALID_PARENT;
        }
    }

    for (uint32_t index = 0U; index < PROCESS_LIMIT; ++index) {
        struct process_core_slot *slot = &core->slots[index];

        if (slot->state != PROCESS_STATE_FREE) {
            continue;
        }
        if (slot->generation == UINT32_MAX) {
            exhausted = true;
            continue;
        }
        ++slot->generation;
        slot->parent = parent;
        slot->address_space = address_space;
        slot->completion_epoch = 1U;
        slot->exit_status = 0;
        slot->task_attachment_count = 0U;
        slot->child_count = 0U;
        slot->state = PROCESS_STATE_RUNNABLE;
        for (size_t reserved = 0U;
             reserved < sizeof(slot->reserved);
             ++reserved) {
            slot->reserved[reserved] = 0U;
        }
        ++core->occupied_count;
        if (parent_slot != NULL) {
            ++parent_slot->child_count;
        }
        *process = make_handle(index, slot->generation);
        return PROCESS_STATUS_OK;
    }
    return exhausted ? PROCESS_STATUS_GENERATION_EXHAUSTED :
        PROCESS_STATUS_NO_SLOTS;
}

enum process_status process_core_attach_task(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_RUNNABLE &&
        slot->state != PROCESS_STATE_BLOCKED) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->task_attachment_count == PROCESS_TASK_ATTACHMENT_LIMIT) {
        return PROCESS_STATUS_TASK_LIMIT;
    }
    ++slot->task_attachment_count;
    return PROCESS_STATUS_OK;
}

enum process_status process_core_detach_task(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state == PROCESS_STATE_DYING) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->task_attachment_count == 0U) {
        return PROCESS_STATUS_NO_TASKS;
    }
    --slot->task_attachment_count;
    if (slot->state == PROCESS_STATE_BLOCKED &&
        slot->task_attachment_count == 0U) {
        slot->state = PROCESS_STATE_RUNNABLE;
    }
    return PROCESS_STATUS_OK;
}

enum process_status process_core_block(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_RUNNABLE) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->task_attachment_count == 0U) {
        return PROCESS_STATUS_NO_TASKS;
    }
    slot->state = PROCESS_STATE_BLOCKED;
    return PROCESS_STATUS_OK;
}

enum process_status process_core_make_runnable(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_BLOCKED) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    slot->state = PROCESS_STATE_RUNNABLE;
    return PROCESS_STATUS_OK;
}

enum process_status process_core_exit(
    struct process_core_state *core,
    process_handle_t process,
    int32_t exit_status
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_RUNNABLE &&
        slot->state != PROCESS_STATE_BLOCKED) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->completion_epoch == UINT64_MAX) {
        return PROCESS_STATUS_EVENT_EPOCH_EXHAUSTED;
    }
    ++slot->completion_epoch;
    slot->exit_status = exit_status;
    slot->state = PROCESS_STATE_EXITED;
    return PROCESS_STATUS_OK;
}

enum process_status process_core_begin_reap(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_EXITED) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->task_attachment_count != 0U || slot->child_count != 0U) {
        return PROCESS_STATUS_PROCESS_BUSY;
    }
    slot->state = PROCESS_STATE_DYING;
    return PROCESS_STATUS_OK;
}

enum process_status process_core_reap(
    struct process_core_state *core,
    process_handle_t process
)
{
    struct process_core_slot *slot;
    struct process_core_slot *parent_slot = NULL;
    enum process_status status = resolve_mutable(core, process, &slot, NULL);
    process_handle_t parent;

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (slot->state != PROCESS_STATE_DYING) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    if (slot->task_attachment_count != 0U || slot->child_count != 0U) {
        return PROCESS_STATUS_PROCESS_BUSY;
    }
    parent = slot->parent;
    if (parent != 0U) {
        status = resolve_mutable(core, parent, &parent_slot, NULL);
        if (status != PROCESS_STATUS_OK || parent_slot->child_count == 0U) {
            core->poisoned = UINT8_C(1);
            return PROCESS_STATUS_CORRUPTED;
        }
    }
    if (parent_slot != NULL) {
        --parent_slot->child_count;
    }
    slot_payload_clear(slot);
    --core->occupied_count;
    return PROCESS_STATUS_OK;
}

static void wait_outputs_clear(
    struct process_wait_token *token,
    int32_t *exit_status
)
{
    if (token != NULL) {
        token->process = 0U;
        token->epoch = 0U;
    }
    if (exit_status != NULL) {
        *exit_status = 0;
    }
}

static enum process_status check_wait_relationship(
    const struct process_core_state *core,
    process_handle_t waiter,
    process_handle_t child,
    const struct process_core_slot **child_slot
)
{
    const struct process_core_slot *waiter_slot;
    enum process_status status = resolve_const(
        core,
        waiter,
        &waiter_slot,
        NULL
    );

    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (waiter_slot->state != PROCESS_STATE_RUNNABLE &&
        waiter_slot->state != PROCESS_STATE_BLOCKED) {
        return PROCESS_STATUS_INVALID_STATE;
    }
    status = resolve_const(core, child, child_slot, NULL);
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if ((*child_slot)->parent != waiter) {
        return PROCESS_STATUS_NOT_PARENT;
    }
    return PROCESS_STATUS_OK;
}

enum process_status process_core_wait_observe(
    const struct process_core_state *core,
    process_handle_t waiter,
    process_handle_t child,
    struct process_wait_token *token,
    int32_t *exit_status
)
{
    const struct process_core_slot *child_slot;
    enum process_status status;

    if (token == NULL || exit_status == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    wait_outputs_clear(token, exit_status);
    status = check_wait_relationship(core, waiter, child, &child_slot);
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    token->process = child;
    token->epoch = child_slot->completion_epoch;
    if (child_slot->state == PROCESS_STATE_EXITED ||
        child_slot->state == PROCESS_STATE_DYING) {
        *exit_status = child_slot->exit_status;
        return PROCESS_STATUS_OK;
    }
    return PROCESS_STATUS_WOULD_BLOCK;
}

enum process_status process_core_wait_complete(
    const struct process_core_state *core,
    process_handle_t waiter,
    struct process_wait_token token,
    int32_t *exit_status
)
{
    const struct process_core_slot *child_slot;
    enum process_status status;

    if (exit_status == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    *exit_status = 0;
    if (token.epoch == 0U) {
        return PROCESS_STATUS_EVENT_CHANGED;
    }
    status = check_wait_relationship(
        core,
        waiter,
        token.process,
        &child_slot
    );
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    if (child_slot->completion_epoch == token.epoch) {
        if (child_slot->state == PROCESS_STATE_EXITED ||
            child_slot->state == PROCESS_STATE_DYING) {
            *exit_status = child_slot->exit_status;
            return PROCESS_STATUS_OK;
        }
        return PROCESS_STATUS_WOULD_BLOCK;
    }
    if (child_slot->state != PROCESS_STATE_EXITED &&
        child_slot->state != PROCESS_STATE_DYING) {
        return PROCESS_STATUS_EVENT_CHANGED;
    }
    *exit_status = child_slot->exit_status;
    return PROCESS_STATUS_OK;
}

static void snapshot_clear(struct process_snapshot *snapshot)
{
    snapshot->state = PROCESS_STATE_FREE;
    snapshot->generation = 0U;
    snapshot->parent = 0U;
    snapshot->address_space = 0U;
    snapshot->completion_epoch = 0U;
    snapshot->task_attachment_count = 0U;
    snapshot->child_count = 0U;
    snapshot->exit_status = 0;
}

enum process_status process_core_snapshot(
    const struct process_core_state *core,
    process_handle_t process,
    struct process_snapshot *snapshot
)
{
    const struct process_core_slot *slot;
    enum process_status status;

    if (snapshot == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    snapshot_clear(snapshot);
    status = resolve_const(core, process, &slot, NULL);
    if (status != PROCESS_STATUS_OK) {
        return status;
    }
    snapshot->state = (enum process_state)slot->state;
    snapshot->generation = slot->generation;
    snapshot->parent = slot->parent;
    snapshot->address_space = slot->address_space;
    snapshot->completion_epoch = slot->completion_epoch;
    snapshot->task_attachment_count = slot->task_attachment_count;
    snapshot->child_count = slot->child_count;
    snapshot->exit_status = slot->exit_status;
    return PROCESS_STATUS_OK;
}

static bool lower_user_address(uint64_t address)
{
    return address >= PROCESS_USER_ADDRESS_MIN &&
        address <= PROCESS_USER_ADDRESS_MAX;
}

static enum process_frame_status validate_frame_common(
    const struct process_user_frame *frame
)
{
    if (frame == NULL) {
        return PROCESS_FRAME_NULL_ARGUMENT;
    }
    if (frame->cs != PROCESS_USER_CODE_SELECTOR ||
        (frame->cs & UINT64_C(3)) != UINT64_C(3)) {
        return PROCESS_FRAME_BAD_CODE_SELECTOR;
    }
    if (frame->ss != PROCESS_USER_DATA_SELECTOR ||
        (frame->ss & UINT64_C(3)) != UINT64_C(3)) {
        return PROCESS_FRAME_BAD_DATA_SELECTOR;
    }
    if (!lower_user_address(frame->rip)) {
        return PROCESS_FRAME_BAD_INSTRUCTION_POINTER;
    }
    if (!lower_user_address(frame->rsp)) {
        return PROCESS_FRAME_BAD_STACK_POINTER;
    }
    if ((frame->rflags & PROCESS_RFLAGS_RESERVED) == 0U ||
        (frame->rflags & PROCESS_RFLAGS_INTERRUPT) == 0U ||
        (frame->rflags & ~PROCESS_RFLAGS_USER_ALLOWED) != 0U) {
        return PROCESS_FRAME_BAD_FLAGS;
    }
    return PROCESS_FRAME_OK;
}

enum process_frame_status process_core_validate_initial_frame(
    const struct process_user_frame *frame
)
{
    enum process_frame_status status = validate_frame_common(frame);

    if (status != PROCESS_FRAME_OK) {
        return status;
    }
    if (frame->rflags !=
        (PROCESS_RFLAGS_RESERVED | PROCESS_RFLAGS_INTERRUPT)) {
        return PROCESS_FRAME_BAD_FLAGS;
    }
    if ((frame->rsp & UINT64_C(15)) != 0U) {
        return PROCESS_FRAME_BAD_STACK_POINTER;
    }
    return PROCESS_FRAME_OK;
}

enum process_frame_status process_core_validate_return_frame(
    const struct process_user_frame *frame
)
{
    return validate_frame_common(frame);
}

enum process_fault_disposition process_core_fault_disposition(uint64_t cs)
{
    return (cs & UINT64_C(3)) == UINT64_C(3) ?
        PROCESS_FAULT_KILL_OFFENDING_PROCESS :
        PROCESS_FAULT_KERNEL_FATAL;
}

bool process_core_resolution_output_self_test(void)
{
    struct process_core_state core;
    struct process_core_slot sentinel;
    struct process_core_slot *resolved = &sentinel;
    process_handle_t process = 0U;
    uint32_t resolved_index = UINT32_C(0xA5A5A5A5);

    process_core_clear(&core);
    if (resolve_mutable(
            &core,
            UINT64_C(0x0000000100000001),
            &resolved,
            &resolved_index
        ) != PROCESS_STATUS_NOT_INITIALIZED || resolved != &sentinel ||
        resolved_index != UINT32_C(0xA5A5A5A5)) {
        return false;
    }
    if (resolve_mutable(
            &core,
            UINT64_C(0x0000000100000001),
            NULL,
            &resolved_index
        ) != PROCESS_STATUS_NULL_ARGUMENT ||
        resolved_index != UINT32_C(0xA5A5A5A5)) {
        return false;
    }
    if (process_core_initialize(&core) != PROCESS_STATUS_OK ||
        process_core_create(
            &core,
            0U,
            UINT64_C(0x0000000100000001),
            &process
        ) != PROCESS_STATUS_OK) {
        return false;
    }

    resolved = &sentinel;
    resolved_index = UINT32_C(0xA5A5A5A5);
    if (resolve_mutable(
            &core,
            UINT64_C(0x0000000100000011),
            &resolved,
            &resolved_index
        ) != PROCESS_STATUS_INVALID_HANDLE || resolved != &sentinel ||
        resolved_index != UINT32_C(0xA5A5A5A5)) {
        return false;
    }
    if (resolve_mutable(
            &core,
            process,
            &resolved,
            &resolved_index
        ) != PROCESS_STATUS_OK || resolved != &core.slots[0] ||
        resolved_index != 0U) {
        return false;
    }
    return true;
}

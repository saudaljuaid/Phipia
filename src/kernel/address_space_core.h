/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_ADDRESS_SPACE_CORE_H
#define ZENITH_ADDRESS_SPACE_CORE_H

#include <stdint.h>

#include <zenith/address_space.h>

struct address_space_core_mapping {
    uint64_t virtual_page;
    uint64_t physical_frame;
    uint32_t permissions;
    uint32_t reserved;
};

struct address_space_core_slot {
    struct address_space_core_mapping mappings[ADDRESS_SPACE_PAGE_LIMIT];
    uint64_t table_frames[ADDRESS_SPACE_TABLE_FRAME_LIMIT];
    uint64_t cr3;
    uint32_t generation;
    uint32_t tlb_generation;
    uint16_t mapping_count;
    uint16_t table_frame_count;
    uint32_t reference_count;
    uint16_t active_cpu_mask;
    uint16_t shootdown_required_mask;
    uint16_t shootdown_target_mask;
    uint16_t shootdown_ack_mask;
    uint8_t state;
    uint8_t shootdown_pending;
    uint8_t reserved[2];
};

struct address_space_core_state {
    struct address_space_core_slot slots[ADDRESS_SPACE_LIMIT];
    uint16_t occupied_count;
    uint8_t initialized;
    uint8_t poisoned;
    uint32_t reserved;
};

/* The core is allocation-free and lock-free. Its owner must serialize calls.
 * Published mapping changes accumulate a shootdown-required CPU mask. A CPU
 * deactivation represents a completed CR3 switch and therefore removes that
 * CPU from outstanding shootdown masks. */

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void address_space_core_clear(struct address_space_core_state *core);
enum address_space_status address_space_core_initialize(
    struct address_space_core_state *core
);
enum address_space_status address_space_core_validate(
    const struct address_space_core_state *core
);
enum address_space_status address_space_core_create(
    struct address_space_core_state *core,
    uint64_t root_table_frame,
    address_space_handle_t *handle
);
enum address_space_status address_space_core_own_table_frame(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t table_frame
);
enum address_space_status address_space_core_map(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page,
    uint64_t physical_frame,
    uint32_t permissions
);
enum address_space_status address_space_core_unmap(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page
);
enum address_space_status address_space_core_repermission(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page,
    uint32_t permissions
);
enum address_space_status address_space_core_publish(
    struct address_space_core_state *core,
    address_space_handle_t handle
);
enum address_space_status address_space_core_abort(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_release_bundle *bundle
);
enum address_space_status address_space_core_retain(
    struct address_space_core_state *core,
    address_space_handle_t handle
);
enum address_space_status address_space_core_release(
    struct address_space_core_state *core,
    address_space_handle_t handle
);
enum address_space_status address_space_core_activate_cpu(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index
);
enum address_space_status address_space_core_deactivate_cpu(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index
);
enum address_space_status address_space_core_begin_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_shootdown *shootdown
);
enum address_space_status address_space_core_ack_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index,
    uint32_t generation
);
enum address_space_status address_space_core_complete_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle
);
enum address_space_status address_space_core_destroy(
    struct address_space_core_state *core,
    address_space_handle_t handle
);
enum address_space_status address_space_core_reap(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_release_bundle *bundle
);
enum address_space_status address_space_core_snapshot(
    const struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_snapshot *snapshot
);
enum address_space_status address_space_core_mapping_snapshot(
    const struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t mapping_index,
    struct address_space_mapping_snapshot *snapshot
);

#endif

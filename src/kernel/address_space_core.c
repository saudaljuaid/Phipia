/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "address_space_core.h"

#define ADDRESS_SPACE_HANDLE_SLOT_MASK UINT64_C(0xFFFFFFFF)
#define ADDRESS_SPACE_MAX_PHYSICAL_FRAME UINT64_C(0x000000FFFFFFFFFF)
#define ADDRESS_SPACE_LOW_CANONICAL_PAGE_MAX UINT64_C(0x00000007FFFFFFFF)
#define ADDRESS_SPACE_HIGH_CANONICAL_PAGE_MIN UINT64_C(0x000FFFF800000000)
#define ADDRESS_SPACE_VIRTUAL_PAGE_MAX UINT64_C(0x000FFFFFFFFFFFFF)
#define ADDRESS_SPACE_PERMISSION_MASK UINT32_C(15)

_Static_assert(ADDRESS_SPACE_LIMIT == 16U,
    "address-space masks require exactly 16 CPUs and slots");
_Static_assert(ADDRESS_SPACE_CPU_LIMIT == 16U,
    "address-space CPU masks require exactly 16 CPUs");
_Static_assert(ADDRESS_SPACE_TABLE_FRAME_LIMIT <= UINT16_MAX,
    "table-frame count must fit its storage");
_Static_assert(ADDRESS_SPACE_PAGE_LIMIT <= UINT16_MAX,
    "mapping count must fit its storage");
_Static_assert(ADDRESS_SPACE_REFERENCE_LIMIT < UINT32_MAX,
    "reference storage must represent one corrupt value past the limit");
_Static_assert(sizeof(struct address_space_core_slot) == 936U,
    "address-space slot must preserve its runtime ABI size");
_Static_assert(sizeof(struct address_space_core_state) == 14984U,
    "address-space core must preserve its bounded runtime ABI size");

static bool table_frame_valid(uint64_t frame)
{
    return frame != 0U && frame <= ADDRESS_SPACE_MAX_PHYSICAL_FRAME;
}

static bool physical_frame_valid(uint64_t frame)
{
    return frame <= ADDRESS_SPACE_MAX_PHYSICAL_FRAME;
}

static bool virtual_page_valid(uint64_t page)
{
    return page <= ADDRESS_SPACE_LOW_CANONICAL_PAGE_MAX ||
        (page >= ADDRESS_SPACE_HIGH_CANONICAL_PAGE_MIN &&
         page <= ADDRESS_SPACE_VIRTUAL_PAGE_MAX);
}

static bool permissions_valid(uint64_t virtual_page, uint32_t permissions)
{
    if ((permissions & ~ADDRESS_SPACE_PERMISSION_MASK) != 0U ||
        (permissions & ADDRESS_SPACE_PERMISSION_READ) == 0U) {
        return false;
    }

    if ((permissions & ADDRESS_SPACE_PERMISSION_WRITE) != 0U &&
        (permissions & ADDRESS_SPACE_PERMISSION_EXECUTE) != 0U) {
        return false;
    }

    if ((permissions & ADDRESS_SPACE_PERMISSION_USER) != 0U &&
        virtual_page > ADDRESS_SPACE_LOW_CANONICAL_PAGE_MAX) {
        return false;
    }

    return true;
}

static bool frame_is_owned(
    const struct address_space_core_state *core,
    uint64_t frame
)
{
    for (size_t slot_index = 0U;
         slot_index < ADDRESS_SPACE_LIMIT;
         ++slot_index) {
        const struct address_space_core_slot *slot =
            &core->slots[slot_index];

        if (slot->state == ADDRESS_SPACE_STATE_FREE) {
            continue;
        }
        for (size_t frame_index = 0U;
             frame_index < slot->table_frame_count;
             ++frame_index) {
            if (slot->table_frames[frame_index] == frame) {
                return true;
            }
        }
    }
    return false;
}

static bool frame_is_user_mapped(
    const struct address_space_core_state *core,
    uint64_t frame
)
{
    for (size_t slot_index = 0U;
         slot_index < ADDRESS_SPACE_LIMIT;
         ++slot_index) {
        const struct address_space_core_slot *slot =
            &core->slots[slot_index];

        if (slot->state == ADDRESS_SPACE_STATE_FREE) {
            continue;
        }
        for (size_t mapping_index = 0U;
             mapping_index < slot->mapping_count;
             ++mapping_index) {
            const struct address_space_core_mapping *mapping =
                &slot->mappings[mapping_index];

            if (mapping->physical_frame == frame &&
                (mapping->permissions & ADDRESS_SPACE_PERMISSION_USER) !=
                    0U) {
                return true;
            }
        }
    }
    return false;
}

static address_space_handle_t make_handle(uint32_t slot, uint32_t generation)
{
    return ((uint64_t)generation << 32U) | ((uint64_t)slot + 1U);
}

static bool mapping_is_zero(const struct address_space_core_mapping *mapping)
{
    return mapping->virtual_page == 0U &&
        mapping->physical_frame == 0U &&
        mapping->permissions == 0U &&
        mapping->reserved == 0U;
}

static void mapping_clear(struct address_space_core_mapping *mapping)
{
    mapping->virtual_page = 0U;
    mapping->physical_frame = 0U;
    mapping->permissions = 0U;
    mapping->reserved = 0U;
}

static void slot_clear_payload(struct address_space_core_slot *slot)
{
    for (size_t index = 0U; index < ADDRESS_SPACE_PAGE_LIMIT; ++index) {
        mapping_clear(&slot->mappings[index]);
    }
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++index) {
        slot->table_frames[index] = 0U;
    }

    slot->cr3 = 0U;
    slot->tlb_generation = 0U;
    slot->mapping_count = 0U;
    slot->table_frame_count = 0U;
    slot->reference_count = 0U;
    slot->active_cpu_mask = 0U;
    slot->shootdown_required_mask = 0U;
    slot->shootdown_target_mask = 0U;
    slot->shootdown_ack_mask = 0U;
    slot->state = ADDRESS_SPACE_STATE_FREE;
    slot->shootdown_pending = 0U;
    slot->reserved[0] = 0U;
    slot->reserved[1] = 0U;
}

static bool free_slot_valid(const struct address_space_core_slot *slot)
{
    if (slot->cr3 != 0U || slot->tlb_generation != 0U ||
        slot->mapping_count != 0U || slot->table_frame_count != 0U ||
        slot->reference_count != 0U || slot->active_cpu_mask != 0U ||
        slot->shootdown_required_mask != 0U ||
        slot->shootdown_target_mask != 0U ||
        slot->shootdown_ack_mask != 0U ||
        slot->shootdown_pending != 0U || slot->reserved[0] != 0U ||
        slot->reserved[1] != 0U) {
        return false;
    }

    for (size_t index = 0U; index < ADDRESS_SPACE_PAGE_LIMIT; ++index) {
        if (!mapping_is_zero(&slot->mappings[index])) {
            return false;
        }
    }
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++index) {
        if (slot->table_frames[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool occupied_slot_valid(const struct address_space_core_slot *slot)
{
    if (slot->generation == 0U || slot->tlb_generation == 0U ||
        slot->mapping_count > ADDRESS_SPACE_PAGE_LIMIT ||
        slot->table_frame_count == 0U ||
        slot->table_frame_count > ADDRESS_SPACE_TABLE_FRAME_LIMIT ||
        slot->reference_count > ADDRESS_SPACE_REFERENCE_LIMIT ||
        slot->shootdown_pending > UINT8_C(1) ||
        slot->reserved[0] != 0U || slot->reserved[1] != 0U ||
        !table_frame_valid(slot->table_frames[0]) ||
        slot->cr3 != (slot->table_frames[0] << 12U)) {
        return false;
    }

    if ((slot->shootdown_required_mask &
         (uint16_t)~slot->active_cpu_mask) != 0U ||
        (slot->shootdown_target_mask &
         (uint16_t)~slot->shootdown_required_mask) != 0U ||
        (slot->shootdown_ack_mask &
         (uint16_t)~slot->shootdown_target_mask) != 0U) {
        return false;
    }

    if (slot->shootdown_pending == 0U &&
        (slot->shootdown_target_mask != 0U ||
         slot->shootdown_ack_mask != 0U)) {
        return false;
    }

    if (slot->state == ADDRESS_SPACE_STATE_BUILDING &&
        (slot->reference_count != 0U || slot->active_cpu_mask != 0U ||
         slot->shootdown_required_mask != 0U ||
         slot->shootdown_target_mask != 0U ||
         slot->shootdown_ack_mask != 0U ||
         slot->shootdown_pending != 0U)) {
        return false;
    }

    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++index) {
        if (index < slot->table_frame_count) {
            if (!table_frame_valid(slot->table_frames[index])) {
                return false;
            }
            for (size_t other = 0U; other < index; ++other) {
                if (slot->table_frames[other] == slot->table_frames[index]) {
                    return false;
                }
            }
        } else if (slot->table_frames[index] != 0U) {
            return false;
        }
    }

    for (size_t index = 0U; index < ADDRESS_SPACE_PAGE_LIMIT; ++index) {
        const struct address_space_core_mapping *mapping =
            &slot->mappings[index];

        if (index < slot->mapping_count) {
            if (!virtual_page_valid(mapping->virtual_page) ||
                !physical_frame_valid(mapping->physical_frame) ||
                !permissions_valid(
                    mapping->virtual_page,
                    mapping->permissions
                ) || mapping->reserved != 0U) {
                return false;
            }
            for (size_t other = 0U; other < index; ++other) {
                if (slot->mappings[other].virtual_page ==
                    mapping->virtual_page) {
                    return false;
                }
            }
        } else if (!mapping_is_zero(mapping)) {
            return false;
        }
    }

    return true;
}

void address_space_core_clear(struct address_space_core_state *core)
{
    if (core == NULL) {
        return;
    }

    for (size_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        core->slots[index].generation = 0U;
        slot_clear_payload(&core->slots[index]);
    }
    core->occupied_count = 0U;
    core->initialized = 0U;
    core->poisoned = 0U;
    core->reserved = 0U;
}

enum address_space_status address_space_core_validate(
    const struct address_space_core_state *core
)
{
    uint16_t occupied = 0U;

    if (core == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    if (core->poisoned > UINT8_C(1)) {
        return ADDRESS_SPACE_STATUS_CORRUPTED;
    }
    if (core->poisoned != 0U) {
        return ADDRESS_SPACE_STATUS_POISONED;
    }
    if (core->initialized == 0U) {
        return ADDRESS_SPACE_STATUS_NOT_INITIALIZED;
    }
    if (core->initialized != UINT8_C(1) || core->reserved != 0U ||
        core->occupied_count > ADDRESS_SPACE_LIMIT) {
        return ADDRESS_SPACE_STATUS_CORRUPTED;
    }

    for (size_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        const struct address_space_core_slot *slot = &core->slots[index];

        if (slot->state == ADDRESS_SPACE_STATE_FREE) {
            if (!free_slot_valid(slot)) {
                return ADDRESS_SPACE_STATUS_CORRUPTED;
            }
        } else if (slot->state == ADDRESS_SPACE_STATE_BUILDING ||
                   slot->state == ADDRESS_SPACE_STATE_PUBLISHED ||
                   slot->state == ADDRESS_SPACE_STATE_DYING) {
            if (!occupied_slot_valid(slot)) {
                return ADDRESS_SPACE_STATUS_CORRUPTED;
            }
            ++occupied;
        } else {
            return ADDRESS_SPACE_STATUS_CORRUPTED;
        }
    }

    if (occupied != core->occupied_count) {
        return ADDRESS_SPACE_STATUS_CORRUPTED;
    }

    for (size_t slot_index = 0U;
         slot_index < ADDRESS_SPACE_LIMIT;
         ++slot_index) {
        const struct address_space_core_slot *slot =
            &core->slots[slot_index];

        if (slot->state == ADDRESS_SPACE_STATE_FREE) {
            continue;
        }
        for (size_t frame_index = 0U;
             frame_index < slot->table_frame_count;
             ++frame_index) {
            uint64_t frame = slot->table_frames[frame_index];

            for (size_t other_slot = 0U;
                 other_slot < slot_index;
                 ++other_slot) {
                const struct address_space_core_slot *other =
                    &core->slots[other_slot];

                for (size_t other_frame = 0U;
                     other_frame < other->table_frame_count;
                     ++other_frame) {
                    if (other->table_frames[other_frame] == frame) {
                        return ADDRESS_SPACE_STATUS_CORRUPTED;
                    }
                }
            }
            if (frame_is_user_mapped(core, frame)) {
                return ADDRESS_SPACE_STATUS_CORRUPTED;
            }
        }
    }
    return ADDRESS_SPACE_STATUS_OK;
}

static enum address_space_status mutation_gate(
    struct address_space_core_state *core
)
{
    enum address_space_status status;

    if (core == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    status = address_space_core_validate(core);
    if (status == ADDRESS_SPACE_STATUS_CORRUPTED) {
        core->poisoned = UINT8_C(1);
    }
    return status;
}

static enum address_space_status resolve_mutable(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_core_slot **slot
)
{
    enum address_space_status status = mutation_gate(core);
    uint64_t encoded_slot;
    uint32_t generation;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }

    encoded_slot = handle & ADDRESS_SPACE_HANDLE_SLOT_MASK;
    generation = (uint32_t)(handle >> 32U);
    if (encoded_slot == 0U || encoded_slot > ADDRESS_SPACE_LIMIT ||
        generation == 0U) {
        return ADDRESS_SPACE_STATUS_INVALID_HANDLE;
    }

    *slot = &core->slots[encoded_slot - 1U];
    if ((*slot)->state == ADDRESS_SPACE_STATE_FREE ||
        (*slot)->generation != generation) {
        return ADDRESS_SPACE_STATUS_STALE_HANDLE;
    }
    return ADDRESS_SPACE_STATUS_OK;
}

static enum address_space_status resolve_const(
    const struct address_space_core_state *core,
    address_space_handle_t handle,
    const struct address_space_core_slot **slot
)
{
    enum address_space_status status = address_space_core_validate(core);
    uint64_t encoded_slot;
    uint32_t generation;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }

    encoded_slot = handle & ADDRESS_SPACE_HANDLE_SLOT_MASK;
    generation = (uint32_t)(handle >> 32U);
    if (encoded_slot == 0U || encoded_slot > ADDRESS_SPACE_LIMIT ||
        generation == 0U) {
        return ADDRESS_SPACE_STATUS_INVALID_HANDLE;
    }

    *slot = &core->slots[encoded_slot - 1U];
    if ((*slot)->state == ADDRESS_SPACE_STATE_FREE ||
        (*slot)->generation != generation) {
        return ADDRESS_SPACE_STATUS_STALE_HANDLE;
    }
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_initialize(
    struct address_space_core_state *core
)
{
    if (core == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    if (core->initialized != 0U) {
        return ADDRESS_SPACE_STATUS_ALREADY_INITIALIZED;
    }

    address_space_core_clear(core);
    core->initialized = UINT8_C(1);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_create(
    struct address_space_core_state *core,
    uint64_t root_table_frame,
    address_space_handle_t *handle
)
{
    enum address_space_status status;
    bool saw_exhausted = false;

    if (handle == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    *handle = 0U;
    status = mutation_gate(core);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (!table_frame_valid(root_table_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_FRAME;
    }
    if (frame_is_owned(core, root_table_frame)) {
        return ADDRESS_SPACE_STATUS_TABLE_FRAME_EXISTS;
    }
    if (frame_is_user_mapped(core, root_table_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_FRAME;
    }

    for (uint32_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        struct address_space_core_slot *slot = &core->slots[index];

        if (slot->state != ADDRESS_SPACE_STATE_FREE) {
            continue;
        }
        if (slot->generation == UINT32_MAX) {
            saw_exhausted = true;
            continue;
        }

        ++slot->generation;
        slot->state = ADDRESS_SPACE_STATE_BUILDING;
        slot->tlb_generation = 1U;
        slot->table_frames[0] = root_table_frame;
        slot->table_frame_count = 1U;
        slot->cr3 = root_table_frame << 12U;
        ++core->occupied_count;
        *handle = make_handle(index, slot->generation);
        return ADDRESS_SPACE_STATUS_OK;
    }

    return saw_exhausted ? ADDRESS_SPACE_STATUS_GENERATION_EXHAUSTED :
        ADDRESS_SPACE_STATUS_NO_SLOTS;
}

enum address_space_status address_space_core_own_table_frame(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t table_frame
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_BUILDING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (!table_frame_valid(table_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_FRAME;
    }
    if (frame_is_owned(core, table_frame)) {
        return ADDRESS_SPACE_STATUS_TABLE_FRAME_EXISTS;
    }
    if (frame_is_user_mapped(core, table_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_FRAME;
    }
    if (slot->table_frame_count == ADDRESS_SPACE_TABLE_FRAME_LIMIT) {
        return ADDRESS_SPACE_STATUS_TABLE_FRAME_LIMIT;
    }

    slot->table_frames[slot->table_frame_count] = table_frame;
    ++slot->table_frame_count;
    return ADDRESS_SPACE_STATUS_OK;
}

static enum address_space_status mapping_mutation_gate(
    struct address_space_core_slot *slot
)
{
    if (slot->state != ADDRESS_SPACE_STATE_BUILDING &&
        slot->state != ADDRESS_SPACE_STATE_PUBLISHED) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->shootdown_pending != 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_IN_PROGRESS;
    }
    if (slot->tlb_generation == UINT32_MAX) {
        return ADDRESS_SPACE_STATUS_GENERATION_EXHAUSTED;
    }
    return ADDRESS_SPACE_STATUS_OK;
}

static void record_mapping_change(struct address_space_core_slot *slot)
{
    ++slot->tlb_generation;
    if (slot->state == ADDRESS_SPACE_STATE_PUBLISHED) {
        slot->shootdown_required_mask |= slot->active_cpu_mask;
    }
}

static uint32_t find_mapping(
    const struct address_space_core_slot *slot,
    uint64_t virtual_page
)
{
    for (uint32_t index = 0U; index < slot->mapping_count; ++index) {
        if (slot->mappings[index].virtual_page == virtual_page) {
            return index;
        }
    }
    return ADDRESS_SPACE_PAGE_LIMIT;
}

enum address_space_status address_space_core_map(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page,
    uint64_t physical_frame,
    uint32_t permissions
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    struct address_space_core_mapping *mapping;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    status = mapping_mutation_gate(slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (!virtual_page_valid(virtual_page)) {
        return ADDRESS_SPACE_STATUS_INVALID_VIRTUAL_PAGE;
    }
    if (!physical_frame_valid(physical_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_FRAME;
    }
    if (!permissions_valid(virtual_page, permissions)) {
        return ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS;
    }
    if ((permissions & ADDRESS_SPACE_PERMISSION_USER) != 0U &&
        frame_is_owned(core, physical_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS;
    }
    if (find_mapping(slot, virtual_page) != ADDRESS_SPACE_PAGE_LIMIT) {
        return ADDRESS_SPACE_STATUS_MAPPING_EXISTS;
    }
    if (slot->mapping_count == ADDRESS_SPACE_PAGE_LIMIT) {
        return ADDRESS_SPACE_STATUS_MAPPING_LIMIT;
    }

    mapping = &slot->mappings[slot->mapping_count];
    mapping->virtual_page = virtual_page;
    mapping->physical_frame = physical_frame;
    mapping->permissions = permissions;
    mapping->reserved = 0U;
    ++slot->mapping_count;
    record_mapping_change(slot);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_unmap(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    uint32_t index;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    status = mapping_mutation_gate(slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (!virtual_page_valid(virtual_page)) {
        return ADDRESS_SPACE_STATUS_INVALID_VIRTUAL_PAGE;
    }
    index = find_mapping(slot, virtual_page);
    if (index == ADDRESS_SPACE_PAGE_LIMIT) {
        return ADDRESS_SPACE_STATUS_MAPPING_NOT_FOUND;
    }

    for (uint32_t next = index + 1U; next < slot->mapping_count; ++next) {
        slot->mappings[next - 1U] = slot->mappings[next];
    }
    --slot->mapping_count;
    mapping_clear(&slot->mappings[slot->mapping_count]);
    record_mapping_change(slot);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_repermission(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint64_t virtual_page,
    uint32_t permissions
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    uint32_t index;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    status = mapping_mutation_gate(slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (!virtual_page_valid(virtual_page)) {
        return ADDRESS_SPACE_STATUS_INVALID_VIRTUAL_PAGE;
    }
    if (!permissions_valid(virtual_page, permissions)) {
        return ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS;
    }
    index = find_mapping(slot, virtual_page);
    if (index == ADDRESS_SPACE_PAGE_LIMIT) {
        return ADDRESS_SPACE_STATUS_MAPPING_NOT_FOUND;
    }
    if ((permissions & ADDRESS_SPACE_PERMISSION_USER) != 0U &&
        frame_is_owned(core, slot->mappings[index].physical_frame)) {
        return ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS;
    }
    if (slot->mappings[index].permissions == permissions) {
        return ADDRESS_SPACE_STATUS_OK;
    }

    slot->mappings[index].permissions = permissions;
    record_mapping_change(slot);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_publish(
    struct address_space_core_state *core,
    address_space_handle_t handle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_BUILDING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    slot->state = ADDRESS_SPACE_STATE_PUBLISHED;
    return ADDRESS_SPACE_STATUS_OK;
}

static void bundle_clear(struct address_space_release_bundle *bundle)
{
    bundle->frame_count = 0U;
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++index) {
        bundle->table_frames[index] = 0U;
    }
}

static void slot_release(
    struct address_space_core_state *core,
    struct address_space_core_slot *slot,
    struct address_space_release_bundle *bundle
)
{
    bundle->frame_count = slot->table_frame_count;
    for (size_t index = 0U; index < slot->table_frame_count; ++index) {
        bundle->table_frames[index] = slot->table_frames[index];
    }
    slot_clear_payload(slot);
    --core->occupied_count;
}

enum address_space_status address_space_core_abort(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_release_bundle *bundle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status;

    if (bundle == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    bundle_clear(bundle);
    status = resolve_mutable(core, handle, &slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_BUILDING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    slot_release(core, slot, bundle);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_retain(
    struct address_space_core_state *core,
    address_space_handle_t handle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->reference_count >= ADDRESS_SPACE_REFERENCE_LIMIT) {
        return ADDRESS_SPACE_STATUS_REFERENCE_OVERFLOW;
    }
    ++slot->reference_count;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_release(
    struct address_space_core_state *core,
    address_space_handle_t handle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED &&
        slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->reference_count == 0U) {
        return ADDRESS_SPACE_STATUS_REFERENCE_UNDERFLOW;
    }
    --slot->reference_count;
    return ADDRESS_SPACE_STATUS_OK;
}

static enum address_space_status cpu_bit(
    uint32_t cpu_index,
    uint16_t *bit
)
{
    if (cpu_index >= ADDRESS_SPACE_CPU_LIMIT) {
        return ADDRESS_SPACE_STATUS_INVALID_CPU;
    }
    *bit = (uint16_t)(UINT16_C(1) << cpu_index);
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_activate_cpu(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    uint16_t bit;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    status = cpu_bit(cpu_index, &bit);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if ((slot->active_cpu_mask & bit) != 0U) {
        return ADDRESS_SPACE_STATUS_CPU_ACTIVE;
    }
    if (slot->shootdown_pending != 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_IN_PROGRESS;
    }
    if (slot->shootdown_required_mask != 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_REQUIRED;
    }
    slot->active_cpu_mask |= bit;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_deactivate_cpu(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    uint16_t bit;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED &&
        slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    status = cpu_bit(cpu_index, &bit);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if ((slot->active_cpu_mask & bit) == 0U) {
        return ADDRESS_SPACE_STATUS_CPU_INACTIVE;
    }

    slot->active_cpu_mask &= (uint16_t)~bit;
    slot->shootdown_required_mask &= (uint16_t)~bit;
    slot->shootdown_target_mask &= (uint16_t)~bit;
    slot->shootdown_ack_mask &= (uint16_t)~bit;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_begin_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_shootdown *shootdown
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status;

    if (shootdown == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    shootdown->generation = 0U;
    shootdown->target_cpu_mask = 0U;
    status = resolve_mutable(core, handle, &slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED &&
        slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->shootdown_pending != 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_IN_PROGRESS;
    }
    if (slot->shootdown_required_mask == 0U) {
        return ADDRESS_SPACE_STATUS_NO_SHOOTDOWN_REQUIRED;
    }

    slot->shootdown_target_mask = slot->shootdown_required_mask;
    slot->shootdown_ack_mask = 0U;
    slot->shootdown_pending = UINT8_C(1);
    shootdown->generation = slot->tlb_generation;
    shootdown->target_cpu_mask = slot->shootdown_target_mask;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_ack_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t cpu_index,
    uint32_t generation
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);
    uint16_t bit;

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED &&
        slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->shootdown_pending == 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_NOT_IN_PROGRESS;
    }
    status = cpu_bit(cpu_index, &bit);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (generation != slot->tlb_generation) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_STALE_GENERATION;
    }
    if ((slot->shootdown_target_mask & bit) == 0U) {
        return ADDRESS_SPACE_STATUS_CPU_NOT_TARGETED;
    }
    if ((slot->shootdown_ack_mask & bit) != 0U) {
        return ADDRESS_SPACE_STATUS_DUPLICATE_ACK;
    }
    slot->shootdown_ack_mask |= bit;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_complete_shootdown(
    struct address_space_core_state *core,
    address_space_handle_t handle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED &&
        slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->shootdown_pending == 0U) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_NOT_IN_PROGRESS;
    }
    if (slot->shootdown_ack_mask != slot->shootdown_target_mask) {
        return ADDRESS_SPACE_STATUS_SHOOTDOWN_INCOMPLETE;
    }

    slot->shootdown_required_mask &=
        (uint16_t)~slot->shootdown_target_mask;
    slot->shootdown_target_mask = 0U;
    slot->shootdown_ack_mask = 0U;
    slot->shootdown_pending = 0U;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_destroy(
    struct address_space_core_state *core,
    address_space_handle_t handle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status = resolve_mutable(core, handle, &slot);

    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_PUBLISHED) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    slot->state = ADDRESS_SPACE_STATE_DYING;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_reap(
    struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_release_bundle *bundle
)
{
    struct address_space_core_slot *slot;
    enum address_space_status status;

    if (bundle == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    bundle_clear(bundle);
    status = resolve_mutable(core, handle, &slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (slot->state != ADDRESS_SPACE_STATE_DYING) {
        return ADDRESS_SPACE_STATUS_INVALID_STATE;
    }
    if (slot->active_cpu_mask != 0U || slot->reference_count != 0U ||
        slot->shootdown_required_mask != 0U ||
        slot->shootdown_target_mask != 0U ||
        slot->shootdown_ack_mask != 0U ||
        slot->shootdown_pending != 0U) {
        return ADDRESS_SPACE_STATUS_SPACE_BUSY;
    }

    slot_release(core, slot, bundle);
    return ADDRESS_SPACE_STATUS_OK;
}

static void snapshot_clear(struct address_space_snapshot *snapshot)
{
    snapshot->state = ADDRESS_SPACE_STATE_FREE;
    snapshot->generation = 0U;
    snapshot->cr3 = 0U;
    snapshot->table_frame_count = 0U;
    snapshot->mapping_count = 0U;
    snapshot->reference_count = 0U;
    snapshot->tlb_generation = 0U;
    snapshot->active_cpu_mask = 0U;
    snapshot->shootdown_required_mask = 0U;
    snapshot->shootdown_target_mask = 0U;
    snapshot->shootdown_ack_mask = 0U;
    snapshot->shootdown_pending = false;
}

enum address_space_status address_space_core_snapshot(
    const struct address_space_core_state *core,
    address_space_handle_t handle,
    struct address_space_snapshot *snapshot
)
{
    const struct address_space_core_slot *slot;
    enum address_space_status status;

    if (snapshot == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    snapshot_clear(snapshot);
    status = resolve_const(core, handle, &slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }

    snapshot->state = (enum address_space_state)slot->state;
    snapshot->generation = slot->generation;
    snapshot->cr3 = slot->cr3;
    snapshot->table_frame_count = slot->table_frame_count;
    snapshot->mapping_count = slot->mapping_count;
    snapshot->reference_count = slot->reference_count;
    snapshot->tlb_generation = slot->tlb_generation;
    snapshot->active_cpu_mask = slot->active_cpu_mask;
    snapshot->shootdown_required_mask = slot->shootdown_required_mask;
    snapshot->shootdown_target_mask = slot->shootdown_target_mask;
    snapshot->shootdown_ack_mask = slot->shootdown_ack_mask;
    snapshot->shootdown_pending = slot->shootdown_pending != 0U;
    return ADDRESS_SPACE_STATUS_OK;
}

enum address_space_status address_space_core_mapping_snapshot(
    const struct address_space_core_state *core,
    address_space_handle_t handle,
    uint32_t mapping_index,
    struct address_space_mapping_snapshot *snapshot
)
{
    const struct address_space_core_slot *slot;
    const struct address_space_core_mapping *mapping;
    enum address_space_status status;

    if (snapshot == NULL) {
        return ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
    }
    snapshot->virtual_page = 0U;
    snapshot->physical_frame = 0U;
    snapshot->permissions = 0U;
    snapshot->user_accessible = false;
    snapshot->writable = false;
    snapshot->executable = false;

    status = resolve_const(core, handle, &slot);
    if (status != ADDRESS_SPACE_STATUS_OK) {
        return status;
    }
    if (mapping_index >= slot->mapping_count) {
        return ADDRESS_SPACE_STATUS_MAPPING_NOT_FOUND;
    }

    mapping = &slot->mappings[mapping_index];
    snapshot->virtual_page = mapping->virtual_page;
    snapshot->physical_frame = mapping->physical_frame;
    snapshot->permissions = mapping->permissions;
    snapshot->user_accessible =
        (mapping->permissions & ADDRESS_SPACE_PERMISSION_USER) != 0U;
    snapshot->writable =
        (mapping->permissions & ADDRESS_SPACE_PERMISSION_WRITE) != 0U;
    snapshot->executable =
        (mapping->permissions & ADDRESS_SPACE_PERMISSION_EXECUTE) != 0U;
    return ADDRESS_SPACE_STATUS_OK;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/address_space_core.h"

#define FNV_OFFSET UINT64_C(1469598103934665603)
#define FNV_PRIME UINT64_C(1099511628211)

static uint64_t digest_mix(uint64_t digest, uint64_t value)
{
    digest ^= value;
    return digest * FNV_PRIME;
}

static uint64_t state_digest(const struct address_space_core_state *core)
{
    uint64_t digest = FNV_OFFSET;

    digest = digest_mix(digest, core->initialized);
    digest = digest_mix(digest, core->poisoned);
    digest = digest_mix(digest, core->occupied_count);
    for (uint32_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        const struct address_space_core_slot *slot = &core->slots[index];

        digest = digest_mix(digest, slot->generation);
        digest = digest_mix(digest, slot->state);
        digest = digest_mix(digest, slot->tlb_generation);
        digest = digest_mix(digest, slot->mapping_count);
        digest = digest_mix(digest, slot->table_frame_count);
        digest = digest_mix(digest, slot->reference_count);
        digest = digest_mix(digest, slot->active_cpu_mask);
        digest = digest_mix(digest, slot->shootdown_required_mask);
        digest = digest_mix(digest, slot->shootdown_target_mask);
        digest = digest_mix(digest, slot->shootdown_ack_mask);
        digest = digest_mix(digest, slot->shootdown_pending);
        digest = digest_mix(digest, slot->cr3);
        for (uint32_t frame = 0U;
             frame < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
             ++frame) {
            digest = digest_mix(digest, slot->table_frames[frame]);
        }
        for (uint32_t mapping = 0U;
             mapping < ADDRESS_SPACE_PAGE_LIMIT;
             ++mapping) {
            digest = digest_mix(
                digest,
                slot->mappings[mapping].virtual_page
            );
            digest = digest_mix(
                digest,
                slot->mappings[mapping].physical_frame
            );
            digest = digest_mix(
                digest,
                slot->mappings[mapping].permissions
            );
        }
    }
    return digest;
}

static uint64_t bundle_digest(const struct address_space_release_bundle *bundle)
{
    uint64_t digest = FNV_OFFSET;

    digest = digest_mix(digest, bundle->frame_count);
    for (uint32_t index = 0U; index < bundle->frame_count; ++index) {
        digest = digest_mix(digest, bundle->table_frames[index]);
    }
    return digest;
}

static bool bundle_is_empty(
    const struct address_space_release_bundle *bundle
)
{
    if (bundle->frame_count != 0U) {
        return false;
    }
    for (uint32_t index = 0U;
         index < ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++index) {
        if (bundle->table_frames[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool null_checks(struct address_space_core_state *core)
{
    address_space_handle_t handle = UINT64_C(1);
    struct address_space_release_bundle bundle = {0};
    struct address_space_shootdown shootdown = {0};
    struct address_space_snapshot snapshot = {0};
    struct address_space_mapping_snapshot mapping = {0};

    address_space_core_clear(NULL);
    return address_space_core_initialize(NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_validate(NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_create(NULL, 1U, &handle) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_create(core, 1U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_own_table_frame(NULL, 1U, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_map(NULL, 1U, 0U, 0U, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_unmap(NULL, 1U, 0U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_repermission(NULL, 1U, 0U, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_publish(NULL, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_abort(NULL, 1U, &bundle) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_abort(core, 1U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_retain(NULL, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_release(NULL, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_activate_cpu(NULL, 1U, 0U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_deactivate_cpu(NULL, 1U, 0U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_begin_shootdown(NULL, 1U, &shootdown) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_begin_shootdown(core, 1U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_ack_shootdown(NULL, 1U, 0U, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_complete_shootdown(NULL, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_destroy(NULL, 1U) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_reap(NULL, 1U, &bundle) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_reap(core, 1U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_snapshot(NULL, 1U, &snapshot) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_snapshot(core, 1U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_mapping_snapshot(NULL, 1U, 0U, &mapping) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        address_space_core_mapping_snapshot(core, 1U, 0U, NULL) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT;
}

static bool failure_output_checks(void)
{
    struct address_space_release_bundle bundle = {
        .frame_count = UINT32_MAX,
        .table_frames = {UINT64_MAX}
    };
    struct address_space_shootdown shootdown = {
        .generation = UINT32_MAX,
        .target_cpu_mask = UINT16_MAX
    };
    struct address_space_snapshot snapshot = {
        .state = ADDRESS_SPACE_STATE_DYING,
        .generation = UINT32_MAX,
        .cr3 = UINT64_MAX,
        .table_frame_count = UINT32_MAX,
        .mapping_count = UINT32_MAX,
        .reference_count = UINT32_MAX,
        .tlb_generation = UINT32_MAX,
        .active_cpu_mask = UINT16_MAX,
        .shootdown_required_mask = UINT16_MAX,
        .shootdown_target_mask = UINT16_MAX,
        .shootdown_ack_mask = UINT16_MAX,
        .shootdown_pending = true
    };
    struct address_space_mapping_snapshot mapping = {
        .virtual_page = UINT64_MAX,
        .physical_frame = UINT64_MAX,
        .permissions = UINT32_MAX,
        .user_accessible = true,
        .writable = true,
        .executable = true
    };

    if (address_space_core_abort(NULL, 1U, &bundle) !=
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT ||
        !bundle_is_empty(&bundle) ||
        address_space_core_begin_shootdown(NULL, 1U, &shootdown) !=
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT ||
        shootdown.generation != 0U || shootdown.target_cpu_mask != 0U ||
        address_space_core_snapshot(NULL, 1U, &snapshot) !=
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT ||
        snapshot.state != ADDRESS_SPACE_STATE_FREE ||
        snapshot.generation != 0U || snapshot.cr3 != 0U ||
        snapshot.table_frame_count != 0U || snapshot.mapping_count != 0U ||
        snapshot.reference_count != 0U || snapshot.tlb_generation != 0U ||
        snapshot.active_cpu_mask != 0U ||
        snapshot.shootdown_required_mask != 0U ||
        snapshot.shootdown_target_mask != 0U ||
        snapshot.shootdown_ack_mask != 0U || snapshot.shootdown_pending ||
        address_space_core_mapping_snapshot(NULL, 1U, 0U, &mapping) !=
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT ||
        mapping.virtual_page != 0U || mapping.physical_frame != 0U ||
        mapping.permissions != 0U || mapping.user_accessible ||
        mapping.writable || mapping.executable) {
        return false;
    }

    bundle.frame_count = UINT32_MAX;
    bundle.table_frames[0] = UINT64_MAX;
    return address_space_core_reap(NULL, 1U, &bundle) ==
            ADDRESS_SPACE_STATUS_NULL_ARGUMENT &&
        bundle_is_empty(&bundle);
}

static bool corruption_checks(void)
{
    struct address_space_core_state core;
    address_space_handle_t handle;

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    core.occupied_count = 2U;
    if (address_space_core_validate(&core) != ADDRESS_SPACE_STATUS_CORRUPTED ||
        address_space_core_publish(&core, handle) !=
            ADDRESS_SPACE_STATUS_CORRUPTED || core.poisoned != 1U) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    core.slots[0].mappings[0].permissions = 1U;
    if (address_space_core_validate(&core) !=
        ADDRESS_SPACE_STATUS_CORRUPTED) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_publish(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    core.slots[0].reference_count = ADDRESS_SPACE_REFERENCE_LIMIT + 1U;
    if (address_space_core_validate(&core) !=
            ADDRESS_SPACE_STATUS_CORRUPTED ||
        address_space_core_retain(&core, handle) !=
            ADDRESS_SPACE_STATUS_CORRUPTED ||
        core.poisoned != 1U) {
        return false;
    }

    return address_space_core_validate(&core) ==
        ADDRESS_SPACE_STATUS_POISONED;
}

static bool edge_checks(void)
{
    struct address_space_core_state core;
    struct address_space_release_bundle bundle = {0};
    struct address_space_shootdown shootdown = {0};
    address_space_handle_t handles[ADDRESS_SPACE_LIMIT];
    address_space_handle_t handle;

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    for (uint32_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        if (address_space_core_create(&core, index + 1U, &handles[index]) !=
            ADDRESS_SPACE_STATUS_OK) {
            return false;
        }
    }
    if (address_space_core_create(&core, 100U, &handle) !=
        ADDRESS_SPACE_STATUS_NO_SLOTS) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    for (uint32_t index = 0U; index < ADDRESS_SPACE_LIMIT; ++index) {
        core.slots[index].generation = UINT32_MAX;
    }
    if (address_space_core_create(&core, 1U, &handle) !=
        ADDRESS_SPACE_STATUS_GENERATION_EXHAUSTED) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    for (uint64_t frame = 2U;
         frame <= ADDRESS_SPACE_TABLE_FRAME_LIMIT;
         ++frame) {
        if (address_space_core_own_table_frame(&core, handle, frame) !=
            ADDRESS_SPACE_STATUS_OK) {
            return false;
        }
    }
    if (address_space_core_own_table_frame(
            &core,
            handle,
            ADDRESS_SPACE_TABLE_FRAME_LIMIT + 1U
        ) != ADDRESS_SPACE_STATUS_TABLE_FRAME_LIMIT) {
        return false;
    }
    for (uint64_t page = 0U; page < ADDRESS_SPACE_PAGE_LIMIT; ++page) {
        if (address_space_core_map(
                &core,
                handle,
                page,
                page,
                ADDRESS_SPACE_PERMISSION_READ
            ) != ADDRESS_SPACE_STATUS_OK) {
            return false;
        }
    }
    if (address_space_core_map(
            &core,
            handle,
            100U,
            100U,
            ADDRESS_SPACE_PERMISSION_READ
        ) != ADDRESS_SPACE_STATUS_MAPPING_LIMIT) {
        return false;
    }

    if (address_space_core_publish(&core, handle) !=
        ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    core.slots[0].reference_count = ADDRESS_SPACE_REFERENCE_LIMIT;
    if (address_space_core_retain(&core, handle) !=
        ADDRESS_SPACE_STATUS_REFERENCE_OVERFLOW) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_map(
            &core,
            handle,
            0U,
            2U,
            ADDRESS_SPACE_PERMISSION_READ | ADDRESS_SPACE_PERMISSION_USER
        ) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 2U, &handles[0]) !=
            ADDRESS_SPACE_STATUS_INVALID_FRAME ||
        address_space_core_map(
            &core,
            handle,
            1U,
            1U,
            ADDRESS_SPACE_PERMISSION_READ | ADDRESS_SPACE_PERMISSION_USER
        ) != ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS ||
        address_space_core_map(
            &core,
            handle,
            1U,
            1U,
            ADDRESS_SPACE_PERMISSION_READ
        ) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_repermission(
            &core,
            handle,
            1U,
            ADDRESS_SPACE_PERMISSION_READ | ADDRESS_SPACE_PERMISSION_USER
        ) != ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS ||
        address_space_core_create(&core, 3U, &handles[0]) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_own_table_frame(&core, handles[0], 1U) !=
            ADDRESS_SPACE_STATUS_TABLE_FRAME_EXISTS) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 1U, &handle) !=
            ADDRESS_SPACE_STATUS_OK) {
        return false;
    }
    core.slots[0].tlb_generation = UINT32_MAX;
    if (address_space_core_map(
            &core,
            handle,
            0U,
            0U,
            ADDRESS_SPACE_PERMISSION_READ
        ) != ADDRESS_SPACE_STATUS_GENERATION_EXHAUSTED) {
        return false;
    }

    address_space_core_clear(&core);
    if (address_space_core_initialize(&core) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_create(&core, 11U, &handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_publish(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_retain(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_activate_cpu(&core, handle, 0U) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_map(
            &core,
            handle,
            0U,
            0U,
            ADDRESS_SPACE_PERMISSION_READ
        ) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_destroy(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_reap(&core, handle, &bundle) !=
            ADDRESS_SPACE_STATUS_SPACE_BUSY ||
        address_space_core_begin_shootdown(&core, handle, &shootdown) !=
            ADDRESS_SPACE_STATUS_OK || shootdown.target_cpu_mask != 1U ||
        address_space_core_ack_shootdown(
            &core,
            handle,
            0U,
            shootdown.generation
        ) != ADDRESS_SPACE_STATUS_OK ||
        address_space_core_complete_shootdown(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_deactivate_cpu(&core, handle, 0U) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_release(&core, handle) !=
            ADDRESS_SPACE_STATUS_OK ||
        address_space_core_reap(&core, handle, &bundle) !=
            ADDRESS_SPACE_STATUS_OK || bundle.frame_count != 1U ||
        bundle.table_frames[0] != 11U) {
        return false;
    }

    return true;
}

static void print_status(
    char operation,
    enum address_space_status status
)
{
    printf("%c %d\n", operation, (int)status);
}

int main(void)
{
    struct address_space_core_state core;
    char operation;

    address_space_core_clear(&core);
    while (scanf(" %c", &operation) == 1) {
        address_space_handle_t handle;
        enum address_space_status status;
        uint64_t first;
        uint64_t second;
        uint32_t value;
        uint32_t generation;

        switch (operation) {
        case 'Z':
            address_space_core_clear(&core);
            puts("Z");
            break;
        case 'I':
            print_status(
                operation,
                address_space_core_initialize(&core)
            );
            break;
        case 'C':
            if (scanf(" %" SCNu64, &first) != 1) {
                return 2;
            }
            handle = 0U;
            status = address_space_core_create(&core, first, &handle);
            printf(
                "C %d %" PRIu64 "\n",
                (int)status,
                handle
            );
            break;
        case 'T':
            if (scanf(" %" SCNu64 " %" SCNu64, &handle, &first) != 2) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_own_table_frame(&core, handle, first)
            );
            break;
        case 'M':
            if (scanf(
                    " %" SCNu64 " %" SCNu64 " %" SCNu64 " %u",
                    &handle,
                    &first,
                    &second,
                    &value
                ) != 4) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_map(
                    &core,
                    handle,
                    first,
                    second,
                    value
                )
            );
            break;
        case 'U':
            if (scanf(" %" SCNu64 " %" SCNu64, &handle, &first) != 2) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_unmap(&core, handle, first)
            );
            break;
        case 'P':
            if (scanf(
                    " %" SCNu64 " %" SCNu64 " %u",
                    &handle,
                    &first,
                    &value
                ) != 3) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_repermission(
                    &core,
                    handle,
                    first,
                    value
                )
            );
            break;
        case 'B':
            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_publish(&core, handle)
            );
            break;
        case 'A':
        case 'E': {
            struct address_space_release_bundle bundle = {0};

            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            status = operation == 'A' ?
                address_space_core_abort(&core, handle, &bundle) :
                address_space_core_reap(&core, handle, &bundle);
            printf(
                "%c %d %u %" PRIu64 "\n",
                operation,
                (int)status,
                bundle.frame_count,
                bundle_digest(&bundle)
            );
            break;
        }
        case 'R':
        case 'L':
        case 'Q':
        case 'Y':
            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            if (operation == 'R') {
                status = address_space_core_retain(&core, handle);
            } else if (operation == 'L') {
                status = address_space_core_release(&core, handle);
            } else if (operation == 'Q') {
                status = address_space_core_complete_shootdown(&core, handle);
            } else {
                status = address_space_core_destroy(&core, handle);
            }
            print_status(operation, status);
            break;
        case 'V':
        case 'D':
            if (scanf(" %" SCNu64 " %u", &handle, &value) != 2) {
                return 2;
            }
            status = operation == 'V' ?
                address_space_core_activate_cpu(&core, handle, value) :
                address_space_core_deactivate_cpu(&core, handle, value);
            print_status(operation, status);
            break;
        case 'S': {
            struct address_space_shootdown shootdown = {0};

            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            status = address_space_core_begin_shootdown(
                &core,
                handle,
                &shootdown
            );
            printf(
                "S %d %u %u\n",
                (int)status,
                shootdown.generation,
                (unsigned int)shootdown.target_cpu_mask
            );
            break;
        }
        case 'K':
            if (scanf(
                    " %" SCNu64 " %u %u",
                    &handle,
                    &value,
                    &generation
                ) != 3) {
                return 2;
            }
            print_status(
                operation,
                address_space_core_ack_shootdown(
                    &core,
                    handle,
                    value,
                    generation
                )
            );
            break;
        case 'G': {
            struct address_space_snapshot snapshot = {0};

            if (scanf(" %" SCNu64, &handle) != 1) {
                return 2;
            }
            status = address_space_core_snapshot(&core, handle, &snapshot);
            printf(
                "G %d %d %u %" PRIu64 " %u %u %u %u %u %u %u %u %u\n",
                (int)status,
                (int)snapshot.state,
                snapshot.generation,
                snapshot.cr3,
                snapshot.table_frame_count,
                snapshot.mapping_count,
                snapshot.reference_count,
                snapshot.tlb_generation,
                (unsigned int)snapshot.active_cpu_mask,
                (unsigned int)snapshot.shootdown_required_mask,
                (unsigned int)snapshot.shootdown_target_mask,
                (unsigned int)snapshot.shootdown_ack_mask,
                snapshot.shootdown_pending ? 1U : 0U
            );
            break;
        }
        case 'N': {
            struct address_space_mapping_snapshot snapshot = {0};

            if (scanf(" %" SCNu64 " %u", &handle, &value) != 2) {
                return 2;
            }
            status = address_space_core_mapping_snapshot(
                &core,
                handle,
                value,
                &snapshot
            );
            printf(
                "N %d %" PRIu64 " %" PRIu64 " %u %u %u %u\n",
                (int)status,
                snapshot.virtual_page,
                snapshot.physical_frame,
                snapshot.permissions,
                snapshot.user_accessible ? 1U : 0U,
                snapshot.writable ? 1U : 0U,
                snapshot.executable ? 1U : 0U
            );
            break;
        }
        case 'X':
            printf(
                "X %d %" PRIu64 "\n",
                (int)address_space_core_validate(&core),
                state_digest(&core)
            );
            break;
        case 'H':
            printf(
                "H %u\n",
                (null_checks(&core) && failure_output_checks() &&
                 corruption_checks() && edge_checks()) ?
                    1U : 0U
            );
            break;
        default:
            return 3;
        }
    }
    return 0;
}

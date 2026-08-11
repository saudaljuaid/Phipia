/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_ADDRESS_SPACE_H
#define ZENITH_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stdint.h>

#define ADDRESS_SPACE_LIMIT UINT32_C(16)
#define ADDRESS_SPACE_PAGE_LIMIT UINT32_C(32)
#define ADDRESS_SPACE_PROCESS_PAGE_LIMIT ADDRESS_SPACE_PAGE_LIMIT
#define ADDRESS_SPACE_TABLE_FRAME_LIMIT UINT32_C(16)
#define ADDRESS_SPACE_CPU_LIMIT UINT32_C(16)
#define ADDRESS_SPACE_REFERENCE_LIMIT UINT32_C(65535)

typedef uint64_t address_space_handle_t;

enum address_space_state {
    ADDRESS_SPACE_STATE_FREE = 0,
    ADDRESS_SPACE_STATE_BUILDING,
    ADDRESS_SPACE_STATE_PUBLISHED,
    ADDRESS_SPACE_STATE_DYING
};

enum address_space_permission {
    ADDRESS_SPACE_PERMISSION_READ = UINT32_C(1) << 0,
    ADDRESS_SPACE_PERMISSION_WRITE = UINT32_C(1) << 1,
    ADDRESS_SPACE_PERMISSION_EXECUTE = UINT32_C(1) << 2,
    ADDRESS_SPACE_PERMISSION_USER = UINT32_C(1) << 3
};

enum address_space_status {
    ADDRESS_SPACE_STATUS_OK = 0,
    ADDRESS_SPACE_STATUS_NULL_ARGUMENT,
    ADDRESS_SPACE_STATUS_ALREADY_INITIALIZED,
    ADDRESS_SPACE_STATUS_NOT_INITIALIZED,
    ADDRESS_SPACE_STATUS_POISONED,
    ADDRESS_SPACE_STATUS_INVALID_HANDLE,
    ADDRESS_SPACE_STATUS_STALE_HANDLE,
    ADDRESS_SPACE_STATUS_NO_SLOTS,
    ADDRESS_SPACE_STATUS_GENERATION_EXHAUSTED,
    ADDRESS_SPACE_STATUS_INVALID_STATE,
    ADDRESS_SPACE_STATUS_INVALID_FRAME,
    ADDRESS_SPACE_STATUS_INVALID_VIRTUAL_PAGE,
    ADDRESS_SPACE_STATUS_INVALID_PERMISSIONS,
    ADDRESS_SPACE_STATUS_MAPPING_EXISTS,
    ADDRESS_SPACE_STATUS_MAPPING_NOT_FOUND,
    ADDRESS_SPACE_STATUS_MAPPING_LIMIT,
    ADDRESS_SPACE_STATUS_TABLE_FRAME_EXISTS,
    ADDRESS_SPACE_STATUS_TABLE_FRAME_LIMIT,
    ADDRESS_SPACE_STATUS_REFERENCE_OVERFLOW,
    ADDRESS_SPACE_STATUS_REFERENCE_UNDERFLOW,
    ADDRESS_SPACE_STATUS_INVALID_CPU,
    ADDRESS_SPACE_STATUS_CPU_ACTIVE,
    ADDRESS_SPACE_STATUS_CPU_INACTIVE,
    ADDRESS_SPACE_STATUS_SHOOTDOWN_REQUIRED,
    ADDRESS_SPACE_STATUS_SHOOTDOWN_IN_PROGRESS,
    ADDRESS_SPACE_STATUS_NO_SHOOTDOWN_REQUIRED,
    ADDRESS_SPACE_STATUS_SHOOTDOWN_NOT_IN_PROGRESS,
    ADDRESS_SPACE_STATUS_SHOOTDOWN_STALE_GENERATION,
    ADDRESS_SPACE_STATUS_CPU_NOT_TARGETED,
    ADDRESS_SPACE_STATUS_DUPLICATE_ACK,
    ADDRESS_SPACE_STATUS_SHOOTDOWN_INCOMPLETE,
    ADDRESS_SPACE_STATUS_SPACE_BUSY,
    ADDRESS_SPACE_STATUS_CORRUPTED
};

struct address_space_mapping_snapshot {
    uint64_t virtual_page;
    uint64_t physical_frame;
    uint32_t permissions;
    bool user_accessible;
    bool writable;
    bool executable;
};

struct address_space_snapshot {
    enum address_space_state state;
    uint32_t generation;
    uint64_t cr3;
    uint32_t table_frame_count;
    uint32_t mapping_count;
    uint32_t reference_count;
    uint32_t tlb_generation;
    uint16_t active_cpu_mask;
    uint16_t shootdown_required_mask;
    uint16_t shootdown_target_mask;
    uint16_t shootdown_ack_mask;
    bool shootdown_pending;
};

struct address_space_shootdown {
    uint32_t generation;
    uint16_t target_cpu_mask;
};

/* Only page-table frames are owned by an address space. Mapping a backing
 * frame never transfers ownership of that frame. */
struct address_space_release_bundle {
    uint32_t frame_count;
    uint64_t table_frames[ADDRESS_SPACE_TABLE_FRAME_LIMIT];
};

#endif

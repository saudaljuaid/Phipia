/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_ADDRESS_SPACE_TABLES_H
#define ZENITH_ADDRESS_SPACE_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADDRESS_SPACE_TABLE_PAGE_SIZE UINT64_C(4096)
#define ADDRESS_SPACE_TABLE_ENTRY_COUNT ((size_t)512U)
#define ADDRESS_SPACE_TABLE_PAGE_COUNT ((size_t)4U)
#define ADDRESS_SPACE_TABLE_MAPPING_LIMIT UINT32_C(32)

#define ADDRESS_SPACE_TABLE_USER_BASE UINT64_C(0x0000000040000000)
#define ADDRESS_SPACE_TABLE_USER_END UINT64_C(0x0000000040200000)
#define ADDRESS_SPACE_TABLE_STACK_GUARD UINT64_C(0x00000000401FE000)
#define ADDRESS_SPACE_TABLE_STACK_PAGE UINT64_C(0x00000000401FF000)
#define ADDRESS_SPACE_TABLE_STACK_TOP ADDRESS_SPACE_TABLE_USER_END

enum address_space_table_page_index {
    ADDRESS_SPACE_TABLE_PML4 = 0,
    ADDRESS_SPACE_TABLE_LOW_PDPT,
    ADDRESS_SPACE_TABLE_USER_PD,
    ADDRESS_SPACE_TABLE_USER_PT
};

enum address_space_table_permission {
    ADDRESS_SPACE_TABLE_PERMISSION_READ = UINT32_C(1) << 0,
    ADDRESS_SPACE_TABLE_PERMISSION_WRITE = UINT32_C(1) << 1,
    ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE = UINT32_C(1) << 2,
    ADDRESS_SPACE_TABLE_PERMISSION_USER = UINT32_C(1) << 3
};

enum address_space_table_status {
    ADDRESS_SPACE_TABLE_STATUS_OK = 0,
    ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT,
    ADDRESS_SPACE_TABLE_STATUS_NOT_CLEAR,
    ADDRESS_SPACE_TABLE_STATUS_NOT_INITIALIZED,
    ADDRESS_SPACE_TABLE_STATUS_BAD_ALIGNMENT,
    ADDRESS_SPACE_TABLE_STATUS_BAD_PHYSICAL_ADDRESS,
    ADDRESS_SPACE_TABLE_STATUS_FRAME_ALIAS,
    ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE,
    ADDRESS_SPACE_TABLE_STATUS_NONCANONICAL_ADDRESS,
    ADDRESS_SPACE_TABLE_STATUS_OUTSIDE_USER_APERTURE,
    ADDRESS_SPACE_TABLE_STATUS_NULL_PAGE,
    ADDRESS_SPACE_TABLE_STATUS_GUARD_PAGE,
    ADDRESS_SPACE_TABLE_STATUS_INVALID_PERMISSIONS,
    ADDRESS_SPACE_TABLE_STATUS_MAPPING_EXISTS,
    ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND,
    ADDRESS_SPACE_TABLE_STATUS_MAPPING_LIMIT,
    ADDRESS_SPACE_TABLE_STATUS_MAPPING_MISMATCH,
    ADDRESS_SPACE_TABLE_STATUS_FRAME_IN_USE,
    ADDRESS_SPACE_TABLE_STATUS_CORRUPTED
};

/*
 * Storage is deliberately separate from metadata.  A runtime may place an
 * array of sixteen of these objects in page-aligned kernel BSS and supply the
 * corresponding identity-mapped physical addresses.  Host tests can instead
 * use arbitrary model physical addresses without confusing them with host
 * pointers.
 */
struct address_space_table_storage {
    _Alignas(ADDRESS_SPACE_TABLE_PAGE_SIZE)
        uint64_t entries[ADDRESS_SPACE_TABLE_PAGE_COUNT]
                        [ADDRESS_SPACE_TABLE_ENTRY_COUNT];
};

struct address_space_table_kernel_template {
    const uint64_t *pml4;
    const uint64_t *low_pdpt;
    uint64_t pml4_physical_address;
    uint64_t low_pdpt_physical_address;
    uint64_t physical_address_mask;
};

struct address_space_table_mapping {
    uint64_t physical_address;
    uint32_t permissions;
    bool accessed;
    bool dirty;
    uint8_t reserved[2];
};

struct address_space_tables {
    struct address_space_table_storage *storage;
    const uint64_t *kernel_pml4;
    const uint64_t *kernel_low_pdpt;
    uint64_t table_physical_addresses[ADDRESS_SPACE_TABLE_PAGE_COUNT];
    uint64_t kernel_pml4_physical_address;
    uint64_t kernel_low_pdpt_physical_address;
    uint64_t physical_address_mask;
    uint32_t mapping_count;
    uint8_t initialized;
    uint8_t reserved[3];
};

/* Null arguments are tolerated independently.  Supplied storage is zeroed. */
void address_space_tables_clear(
    struct address_space_tables *tables,
    struct address_space_table_storage *storage
);

/*
 * Build is transactional: every check completes before the first output
 * write.  Both metadata and all four supplied pages must initially be clear.
 */
enum address_space_table_status address_space_tables_build(
    struct address_space_tables *tables,
    struct address_space_table_storage *storage,
    const uint64_t table_physical_addresses[ADDRESS_SPACE_TABLE_PAGE_COUNT],
    const struct address_space_table_kernel_template *kernel_template
);

enum address_space_table_status address_space_tables_validate(
    const struct address_space_tables *tables
);
enum address_space_table_status address_space_tables_map(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
);
enum address_space_table_status address_space_tables_unmap(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint64_t expected_physical_address
);
enum address_space_table_status address_space_tables_repermission(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint32_t permissions
);
enum address_space_table_status address_space_tables_query(
    const struct address_space_tables *tables,
    uint64_t virtual_address,
    struct address_space_table_mapping *mapping
);
const char *address_space_table_status_string(
    enum address_space_table_status status
);

#endif

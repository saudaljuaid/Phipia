/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "address_space_tables.h"

#define TABLE_ENTRY_PRESENT (UINT64_C(1) << 0U)
#define TABLE_ENTRY_WRITABLE (UINT64_C(1) << 1U)
#define TABLE_ENTRY_USER (UINT64_C(1) << 2U)
#define TABLE_ENTRY_WRITE_THROUGH (UINT64_C(1) << 3U)
#define TABLE_ENTRY_CACHE_DISABLE (UINT64_C(1) << 4U)
#define TABLE_ENTRY_ACCESSED (UINT64_C(1) << 5U)
#define TABLE_ENTRY_DIRTY (UINT64_C(1) << 6U)
#define TABLE_ENTRY_HUGE (UINT64_C(1) << 7U)
#define TABLE_ENTRY_NO_EXECUTE (UINT64_C(1) << 63U)
#define TABLE_ENTRY_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

#define TABLE_PARENT_ALLOWED \
    (TABLE_ENTRY_PRESENT | TABLE_ENTRY_WRITABLE | \
     TABLE_ENTRY_WRITE_THROUGH | TABLE_ENTRY_CACHE_DISABLE | \
     TABLE_ENTRY_ACCESSED | TABLE_ENTRY_NO_EXECUTE | \
     TABLE_ENTRY_ADDRESS_MASK)
#define TABLE_LEAF_ALLOWED \
    (TABLE_ENTRY_PRESENT | TABLE_ENTRY_WRITABLE | TABLE_ENTRY_USER | \
     TABLE_ENTRY_ACCESSED | TABLE_ENTRY_DIRTY | TABLE_ENTRY_NO_EXECUTE | \
     TABLE_ENTRY_ADDRESS_MASK)
#define TABLE_PRIVATE_PARENT_FLAGS \
    (TABLE_ENTRY_PRESENT | TABLE_ENTRY_WRITABLE | TABLE_ENTRY_USER)
#define TABLE_PERMISSION_MASK UINT32_C(15)

_Static_assert(
    sizeof(struct address_space_table_storage) ==
        ADDRESS_SPACE_TABLE_PAGE_COUNT * ADDRESS_SPACE_TABLE_PAGE_SIZE,
    "four-level user table storage must occupy exactly four pages"
);
_Static_assert(
    _Alignof(struct address_space_table_storage) ==
        ADDRESS_SPACE_TABLE_PAGE_SIZE,
    "user table storage must be page aligned"
);
_Static_assert(
    sizeof(struct address_space_table_mapping) == 16U,
    "mapping snapshot ABI changed"
);
_Static_assert(
    sizeof(struct address_space_tables) == 88U,
    "table metadata ABI changed"
);
_Static_assert(
    ADDRESS_SPACE_TABLE_USER_END - ADDRESS_SPACE_TABLE_USER_BASE ==
        UINT64_C(0x200000),
    "the bounded user aperture must occupy one page-table leaf page"
);
_Static_assert(
    ADDRESS_SPACE_TABLE_STACK_GUARD + ADDRESS_SPACE_TABLE_PAGE_SIZE ==
        ADDRESS_SPACE_TABLE_STACK_PAGE &&
        ADDRESS_SPACE_TABLE_STACK_PAGE + ADDRESS_SPACE_TABLE_PAGE_SIZE ==
            ADDRESS_SPACE_TABLE_STACK_TOP,
    "the initial user stack must retain one absent guard page"
);

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}
static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;

    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool address_is_page_aligned(uint64_t address)
{
    return (address & (ADDRESS_SPACE_TABLE_PAGE_SIZE - 1U)) == 0U;
}

static bool pointer_is_page_aligned(const void *pointer)
{
    return ((uintptr_t)pointer &
        (uintptr_t)(ADDRESS_SPACE_TABLE_PAGE_SIZE - 1U)) == 0U;
}

static bool address_is_canonical(uint64_t address)
{
    const uint64_t upper = address >> 48U;

    return (address & (UINT64_C(1) << 47U)) == 0U
        ? upper == 0U
        : upper == UINT64_C(0xFFFF);
}

static bool physical_mask_is_valid(uint64_t mask)
{
    const uint64_t page_mask = mask >> 12U;

    return mask != 0U && address_is_page_aligned(mask) &&
        (mask & ~TABLE_ENTRY_ADDRESS_MASK) == 0U &&
        (page_mask & (page_mask + 1U)) == 0U;
}

static bool physical_address_is_valid(uint64_t address, uint64_t mask)
{
    return address != 0U && address_is_page_aligned(address) &&
        (address & ~mask) == 0U;
}

static bool ranges_overlap(
    uintptr_t left,
    size_t left_size,
    uintptr_t right,
    size_t right_size
)
{
    if (left_size == 0U || right_size == 0U ||
        left > UINTPTR_MAX - (left_size - 1U) ||
        right > UINTPTR_MAX - (right_size - 1U)) {
        return true;
    }
    return left <= right + right_size - 1U &&
        right <= left + left_size - 1U;
}

static bool parent_entry_is_valid(
    uint64_t entry,
    uint64_t physical_mask,
    bool allow_execute_disable
)
{
    const uint64_t address = entry & TABLE_ENTRY_ADDRESS_MASK;

    return (entry & TABLE_ENTRY_PRESENT) != 0U &&
        (entry & TABLE_ENTRY_USER) == 0U &&
        (entry & TABLE_ENTRY_HUGE) == 0U &&
        (entry & ~TABLE_PARENT_ALLOWED) == 0U &&
        (allow_execute_disable ||
            (entry & TABLE_ENTRY_NO_EXECUTE) == 0U) &&
        physical_address_is_valid(address, physical_mask);
}

static bool private_parent_matches(uint64_t entry, uint64_t address)
{
    return (entry & ~TABLE_ENTRY_ACCESSED) ==
        (address | TABLE_PRIVATE_PARENT_FLAGS);
}

static bool shared_entry_matches(uint64_t observed, uint64_t expected)
{
    return (observed & ~TABLE_ENTRY_ACCESSED) ==
        (expected & ~TABLE_ENTRY_ACCESSED);
}

static bool physical_matches_any_private_table(
    const uint64_t physical_addresses[ADDRESS_SPACE_TABLE_PAGE_COUNT],
    uint64_t address
)
{
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_PAGE_COUNT;
         ++index) {
        if (physical_addresses[index] == address) {
            return true;
        }
    }
    return false;
}

static bool table_configuration_is_valid(
    const struct address_space_tables *tables,
    const struct address_space_table_storage *storage,
    const uint64_t physical_addresses[ADDRESS_SPACE_TABLE_PAGE_COUNT],
    const struct address_space_table_kernel_template *kernel_template
)
{
    const uintptr_t storage_address = (uintptr_t)(const void *)storage;
    const uintptr_t state_address = (uintptr_t)(const void *)tables;
    const uintptr_t pml4_address =
        (uintptr_t)(const void *)kernel_template->pml4;
    const uintptr_t low_pdpt_address =
        (uintptr_t)(const void *)kernel_template->low_pdpt;
    const uint64_t mask = kernel_template->physical_address_mask;
    uint64_t kernel_pd_address;

    if (!pointer_is_page_aligned(storage) ||
        !pointer_is_page_aligned(kernel_template->pml4) ||
        !pointer_is_page_aligned(kernel_template->low_pdpt)) {
        return false;
    }
    if (ranges_overlap(
            storage_address, sizeof(*storage),
            state_address, sizeof(*tables)
        ) ||
        ranges_overlap(
            state_address, sizeof(*tables),
            pml4_address, ADDRESS_SPACE_TABLE_PAGE_SIZE
        ) ||
        ranges_overlap(
            state_address, sizeof(*tables),
            low_pdpt_address, ADDRESS_SPACE_TABLE_PAGE_SIZE
        ) ||
        ranges_overlap(
            storage_address, sizeof(*storage),
            pml4_address, ADDRESS_SPACE_TABLE_PAGE_SIZE
        ) ||
        ranges_overlap(
            storage_address, sizeof(*storage),
            low_pdpt_address, ADDRESS_SPACE_TABLE_PAGE_SIZE
        ) ||
        ranges_overlap(
            pml4_address, ADDRESS_SPACE_TABLE_PAGE_SIZE,
            low_pdpt_address, ADDRESS_SPACE_TABLE_PAGE_SIZE
        )) {
        return false;
    }
    if (!physical_mask_is_valid(mask) ||
        !physical_address_is_valid(
            kernel_template->pml4_physical_address, mask
        ) ||
        !physical_address_is_valid(
            kernel_template->low_pdpt_physical_address, mask
        ) ||
        kernel_template->pml4_physical_address ==
            kernel_template->low_pdpt_physical_address) {
        return false;
    }

    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_PAGE_COUNT;
         ++index) {
        if (!physical_address_is_valid(physical_addresses[index], mask) ||
            physical_addresses[index] ==
                kernel_template->pml4_physical_address ||
            physical_addresses[index] ==
                kernel_template->low_pdpt_physical_address) {
            return false;
        }
        for (size_t other = 0U; other < index; ++other) {
            if (physical_addresses[other] == physical_addresses[index]) {
                return false;
            }
        }
    }

    if (!parent_entry_is_valid(
            kernel_template->pml4[0], mask, false
        ) ||
        (kernel_template->pml4[0] & TABLE_ENTRY_ADDRESS_MASK) !=
            kernel_template->low_pdpt_physical_address ||
        !parent_entry_is_valid(
            kernel_template->low_pdpt[0], mask, false
        )) {
        return false;
    }
    kernel_pd_address =
        kernel_template->low_pdpt[0] & TABLE_ENTRY_ADDRESS_MASK;
    if (physical_matches_any_private_table(
            physical_addresses, kernel_pd_address
        )) {
        return false;
    }

    for (size_t index = 1U; index < 256U; ++index) {
        if (kernel_template->pml4[index] != 0U) {
            return false;
        }
    }
    for (size_t index = 256U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        const uint64_t entry = kernel_template->pml4[index];

        if (entry == 0U) {
            continue;
        }
        if (!parent_entry_is_valid(entry, mask, true) ||
            physical_matches_any_private_table(
                physical_addresses,
                entry & TABLE_ENTRY_ADDRESS_MASK
            )) {
            return false;
        }
    }
    for (size_t index = 1U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        if (kernel_template->low_pdpt[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum address_space_table_status address_status(
    uint64_t virtual_address
)
{
    if (!address_is_canonical(virtual_address)) {
        return ADDRESS_SPACE_TABLE_STATUS_NONCANONICAL_ADDRESS;
    }
    if (!address_is_page_aligned(virtual_address)) {
        return ADDRESS_SPACE_TABLE_STATUS_BAD_ALIGNMENT;
    }
    if (virtual_address < ADDRESS_SPACE_TABLE_PAGE_SIZE) {
        return ADDRESS_SPACE_TABLE_STATUS_NULL_PAGE;
    }
    if (virtual_address < ADDRESS_SPACE_TABLE_USER_BASE ||
        virtual_address >= ADDRESS_SPACE_TABLE_USER_END) {
        return ADDRESS_SPACE_TABLE_STATUS_OUTSIDE_USER_APERTURE;
    }
    if (virtual_address == ADDRESS_SPACE_TABLE_STACK_GUARD) {
        return ADDRESS_SPACE_TABLE_STATUS_GUARD_PAGE;
    }
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

static bool permissions_are_valid(uint32_t permissions)
{
    return (permissions & ~TABLE_PERMISSION_MASK) == 0U &&
        (permissions & ADDRESS_SPACE_TABLE_PERMISSION_READ) != 0U &&
        (permissions & ADDRESS_SPACE_TABLE_PERMISSION_USER) != 0U &&
        !((permissions & ADDRESS_SPACE_TABLE_PERMISSION_WRITE) != 0U &&
          (permissions & ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE) != 0U);
}

static uint64_t leaf_from_mapping(
    uint64_t physical_address,
    uint32_t permissions,
    uint64_t retained_hardware_bits
)
{
    uint64_t entry = physical_address | TABLE_ENTRY_PRESENT |
        TABLE_ENTRY_USER |
        (retained_hardware_bits &
            (TABLE_ENTRY_ACCESSED | TABLE_ENTRY_DIRTY));

    if ((permissions & ADDRESS_SPACE_TABLE_PERMISSION_WRITE) != 0U) {
        entry |= TABLE_ENTRY_WRITABLE;
    }
    if ((permissions & ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE) == 0U) {
        entry |= TABLE_ENTRY_NO_EXECUTE;
    }
    return entry;
}

static uint32_t permissions_from_leaf(uint64_t entry)
{
    uint32_t permissions = ADDRESS_SPACE_TABLE_PERMISSION_READ |
        ADDRESS_SPACE_TABLE_PERMISSION_USER;

    if ((entry & TABLE_ENTRY_WRITABLE) != 0U) {
        permissions |= ADDRESS_SPACE_TABLE_PERMISSION_WRITE;
    }
    if ((entry & TABLE_ENTRY_NO_EXECUTE) == 0U) {
        permissions |= ADDRESS_SPACE_TABLE_PERMISSION_EXECUTE;
    }
    return permissions;
}

static size_t user_leaf_index(uint64_t virtual_address)
{
    return (size_t)((virtual_address - ADDRESS_SPACE_TABLE_USER_BASE) >> 12U);
}

static void mapping_clear(struct address_space_table_mapping *mapping)
{
    if (mapping != NULL) {
        bytes_zero(mapping, sizeof(*mapping));
    }
}

void address_space_tables_clear(
    struct address_space_tables *tables,
    struct address_space_table_storage *storage
)
{
    if (storage != NULL) {
        bytes_zero(storage, sizeof(*storage));
    }
    if (tables != NULL) {
        bytes_zero(tables, sizeof(*tables));
    }
}

enum address_space_table_status address_space_tables_build(
    struct address_space_tables *tables,
    struct address_space_table_storage *storage,
    const uint64_t physical_addresses[ADDRESS_SPACE_TABLE_PAGE_COUNT],
    const struct address_space_table_kernel_template *kernel_template
)
{
    uint64_t *pml4;
    uint64_t *low_pdpt;
    uint64_t *user_pd;

    if (tables == NULL || storage == NULL || physical_addresses == NULL ||
        kernel_template == NULL || kernel_template->pml4 == NULL ||
        kernel_template->low_pdpt == NULL) {
        return ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT;
    }
    if (!bytes_are_zero(tables, sizeof(*tables)) ||
        !bytes_are_zero(storage, sizeof(*storage))) {
        return ADDRESS_SPACE_TABLE_STATUS_NOT_CLEAR;
    }
    if (!table_configuration_is_valid(
            tables, storage, physical_addresses, kernel_template
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE;
    }

    pml4 = storage->entries[ADDRESS_SPACE_TABLE_PML4];
    low_pdpt = storage->entries[ADDRESS_SPACE_TABLE_LOW_PDPT];
    user_pd = storage->entries[ADDRESS_SPACE_TABLE_USER_PD];

    pml4[0] = physical_addresses[ADDRESS_SPACE_TABLE_LOW_PDPT] |
        TABLE_PRIVATE_PARENT_FLAGS;
    for (size_t index = 256U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        pml4[index] = kernel_template->pml4[index];
    }
    low_pdpt[0] = kernel_template->low_pdpt[0];
    low_pdpt[1] = physical_addresses[ADDRESS_SPACE_TABLE_USER_PD] |
        TABLE_PRIVATE_PARENT_FLAGS;
    user_pd[0] = physical_addresses[ADDRESS_SPACE_TABLE_USER_PT] |
        TABLE_PRIVATE_PARENT_FLAGS;

    tables->storage = storage;
    tables->kernel_pml4 = kernel_template->pml4;
    tables->kernel_low_pdpt = kernel_template->low_pdpt;
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_PAGE_COUNT;
         ++index) {
        tables->table_physical_addresses[index] = physical_addresses[index];
    }
    tables->kernel_pml4_physical_address =
        kernel_template->pml4_physical_address;
    tables->kernel_low_pdpt_physical_address =
        kernel_template->low_pdpt_physical_address;
    tables->physical_address_mask = kernel_template->physical_address_mask;
    tables->mapping_count = 0U;
    tables->initialized = UINT8_C(1);
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

enum address_space_table_status address_space_tables_validate(
    const struct address_space_tables *tables
)
{
    const struct address_space_table_storage *storage;
    const uint64_t *pml4;
    const uint64_t *low_pdpt;
    const uint64_t *user_pd;
    const uint64_t *user_pt;
    struct address_space_table_kernel_template kernel_template;
    uint64_t seen_frames[ADDRESS_SPACE_TABLE_MAPPING_LIMIT];
    uint32_t mapped = 0U;

    if (tables == NULL) {
        return ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT;
    }
    if (tables->initialized == 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_NOT_INITIALIZED;
    }
    if (tables->initialized != UINT8_C(1) || tables->storage == NULL ||
        tables->kernel_pml4 == NULL || tables->kernel_low_pdpt == NULL ||
        tables->mapping_count > ADDRESS_SPACE_TABLE_MAPPING_LIMIT ||
        tables->reserved[0] != 0U || tables->reserved[1] != 0U ||
        tables->reserved[2] != 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
    }

    storage = tables->storage;
    kernel_template.pml4 = tables->kernel_pml4;
    kernel_template.low_pdpt = tables->kernel_low_pdpt;
    kernel_template.pml4_physical_address =
        tables->kernel_pml4_physical_address;
    kernel_template.low_pdpt_physical_address =
        tables->kernel_low_pdpt_physical_address;
    kernel_template.physical_address_mask = tables->physical_address_mask;
    if (!table_configuration_is_valid(
            tables,
            storage,
            tables->table_physical_addresses,
            &kernel_template
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
    }

    pml4 = storage->entries[ADDRESS_SPACE_TABLE_PML4];
    low_pdpt = storage->entries[ADDRESS_SPACE_TABLE_LOW_PDPT];
    user_pd = storage->entries[ADDRESS_SPACE_TABLE_USER_PD];
    user_pt = storage->entries[ADDRESS_SPACE_TABLE_USER_PT];

    if (!private_parent_matches(
            pml4[0],
            tables->table_physical_addresses[
                ADDRESS_SPACE_TABLE_LOW_PDPT]
        ) ||
        !shared_entry_matches(low_pdpt[0], tables->kernel_low_pdpt[0]) ||
        !private_parent_matches(
            low_pdpt[1],
            tables->table_physical_addresses[ADDRESS_SPACE_TABLE_USER_PD]
        ) ||
        !private_parent_matches(
            user_pd[0],
            tables->table_physical_addresses[ADDRESS_SPACE_TABLE_USER_PT]
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
    }

    for (size_t index = 1U; index < 256U; ++index) {
        if (pml4[index] != 0U) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
    }
    for (size_t index = 256U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        if (!shared_entry_matches(pml4[index], tables->kernel_pml4[index])) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
    }
    for (size_t index = 2U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        if (low_pdpt[index] != 0U) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
    }
    for (size_t index = 1U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        if (user_pd[index] != 0U) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
    }

    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        const uint64_t entry = user_pt[index];
        uint64_t physical;

        if (entry == 0U) {
            continue;
        }
        if ((entry & TABLE_ENTRY_PRESENT) == 0U ||
            (entry & TABLE_ENTRY_USER) == 0U ||
            (entry & ~TABLE_LEAF_ALLOWED) != 0U ||
            ((entry & TABLE_ENTRY_WRITABLE) != 0U &&
             (entry & TABLE_ENTRY_NO_EXECUTE) == 0U) ||
            index == user_leaf_index(ADDRESS_SPACE_TABLE_STACK_GUARD)) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
        physical = entry & TABLE_ENTRY_ADDRESS_MASK;
        if (!physical_address_is_valid(
                physical, tables->physical_address_mask
            ) ||
            physical_matches_any_private_table(
                tables->table_physical_addresses, physical
            )) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
        for (uint32_t other = 0U; other < mapped; ++other) {
            if (seen_frames[other] == physical) {
                return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
            }
        }
        if (mapped == ADDRESS_SPACE_TABLE_MAPPING_LIMIT) {
            return ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
        }
        seen_frames[mapped] = physical;
        ++mapped;
    }

    return mapped == tables->mapping_count
        ? ADDRESS_SPACE_TABLE_STATUS_OK
        : ADDRESS_SPACE_TABLE_STATUS_CORRUPTED;
}

enum address_space_table_status address_space_tables_map(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
)
{
    enum address_space_table_status status =
        address_space_tables_validate(tables);
    uint64_t *user_pt;
    size_t leaf_index;

    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    status = address_status(virtual_address);
    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    if (!permissions_are_valid(permissions)) {
        return ADDRESS_SPACE_TABLE_STATUS_INVALID_PERMISSIONS;
    }
    if (!physical_address_is_valid(
            physical_address, tables->physical_address_mask
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_BAD_PHYSICAL_ADDRESS;
    }
    if (physical_matches_any_private_table(
            tables->table_physical_addresses, physical_address
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_FRAME_ALIAS;
    }

    user_pt = tables->storage->entries[ADDRESS_SPACE_TABLE_USER_PT];
    leaf_index = user_leaf_index(virtual_address);
    if (user_pt[leaf_index] != 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_EXISTS;
    }
    for (size_t index = 0U;
         index < ADDRESS_SPACE_TABLE_ENTRY_COUNT;
         ++index) {
        if (user_pt[index] != 0U &&
            (user_pt[index] & TABLE_ENTRY_ADDRESS_MASK) == physical_address) {
            return ADDRESS_SPACE_TABLE_STATUS_FRAME_IN_USE;
        }
    }
    if (tables->mapping_count == ADDRESS_SPACE_TABLE_MAPPING_LIMIT) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_LIMIT;
    }

    user_pt[leaf_index] = leaf_from_mapping(
        physical_address, permissions, 0U
    );
    ++tables->mapping_count;
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

enum address_space_table_status address_space_tables_unmap(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint64_t expected_physical_address
)
{
    enum address_space_table_status status =
        address_space_tables_validate(tables);
    uint64_t *entry;

    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    status = address_status(virtual_address);
    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    if (!physical_address_is_valid(
            expected_physical_address, tables->physical_address_mask
        )) {
        return ADDRESS_SPACE_TABLE_STATUS_BAD_PHYSICAL_ADDRESS;
    }
    entry = &tables->storage->entries[ADDRESS_SPACE_TABLE_USER_PT]
        [user_leaf_index(virtual_address)];
    if (*entry == 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND;
    }
    if ((*entry & TABLE_ENTRY_ADDRESS_MASK) != expected_physical_address) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_MISMATCH;
    }

    *entry = 0U;
    --tables->mapping_count;
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

enum address_space_table_status address_space_tables_repermission(
    struct address_space_tables *tables,
    uint64_t virtual_address,
    uint32_t permissions
)
{
    enum address_space_table_status status =
        address_space_tables_validate(tables);
    uint64_t *entry;

    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    status = address_status(virtual_address);
    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    if (!permissions_are_valid(permissions)) {
        return ADDRESS_SPACE_TABLE_STATUS_INVALID_PERMISSIONS;
    }
    entry = &tables->storage->entries[ADDRESS_SPACE_TABLE_USER_PT]
        [user_leaf_index(virtual_address)];
    if (*entry == 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND;
    }
    *entry = leaf_from_mapping(
        *entry & TABLE_ENTRY_ADDRESS_MASK,
        permissions,
        *entry
    );
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

enum address_space_table_status address_space_tables_query(
    const struct address_space_tables *tables,
    uint64_t virtual_address,
    struct address_space_table_mapping *mapping
)
{
    enum address_space_table_status status;
    uint64_t entry;

    if (mapping == NULL) {
        return ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT;
    }
    mapping_clear(mapping);
    status = address_space_tables_validate(tables);
    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    status = address_status(virtual_address);
    if (status != ADDRESS_SPACE_TABLE_STATUS_OK) {
        return status;
    }
    entry = tables->storage->entries[ADDRESS_SPACE_TABLE_USER_PT]
        [user_leaf_index(virtual_address)];
    if (entry == 0U) {
        return ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND;
    }

    mapping->physical_address = entry & TABLE_ENTRY_ADDRESS_MASK;
    mapping->permissions = permissions_from_leaf(entry);
    mapping->accessed = (entry & TABLE_ENTRY_ACCESSED) != 0U;
    mapping->dirty = (entry & TABLE_ENTRY_DIRTY) != 0U;
    return ADDRESS_SPACE_TABLE_STATUS_OK;
}

const char *address_space_table_status_string(
    enum address_space_table_status status
)
{
    switch (status) {
    case ADDRESS_SPACE_TABLE_STATUS_OK:
        return "ok";
    case ADDRESS_SPACE_TABLE_STATUS_NULL_ARGUMENT:
        return "null argument";
    case ADDRESS_SPACE_TABLE_STATUS_NOT_CLEAR:
        return "metadata or table storage is not clear";
    case ADDRESS_SPACE_TABLE_STATUS_NOT_INITIALIZED:
        return "table state is not initialized";
    case ADDRESS_SPACE_TABLE_STATUS_BAD_ALIGNMENT:
        return "address is not page aligned";
    case ADDRESS_SPACE_TABLE_STATUS_BAD_PHYSICAL_ADDRESS:
        return "physical address is invalid";
    case ADDRESS_SPACE_TABLE_STATUS_FRAME_ALIAS:
        return "backing frame aliases a private table";
    case ADDRESS_SPACE_TABLE_STATUS_INVALID_TEMPLATE:
        return "permanent kernel template is malformed";
    case ADDRESS_SPACE_TABLE_STATUS_NONCANONICAL_ADDRESS:
        return "virtual address is noncanonical";
    case ADDRESS_SPACE_TABLE_STATUS_OUTSIDE_USER_APERTURE:
        return "virtual address is outside the bounded user aperture";
    case ADDRESS_SPACE_TABLE_STATUS_NULL_PAGE:
        return "null page cannot be mapped";
    case ADDRESS_SPACE_TABLE_STATUS_GUARD_PAGE:
        return "initial stack guard must remain absent";
    case ADDRESS_SPACE_TABLE_STATUS_INVALID_PERMISSIONS:
        return "user permissions are invalid";
    case ADDRESS_SPACE_TABLE_STATUS_MAPPING_EXISTS:
        return "virtual page is already mapped";
    case ADDRESS_SPACE_TABLE_STATUS_MAPPING_NOT_FOUND:
        return "virtual page is not mapped";
    case ADDRESS_SPACE_TABLE_STATUS_MAPPING_LIMIT:
        return "bounded mapping limit is exhausted";
    case ADDRESS_SPACE_TABLE_STATUS_MAPPING_MISMATCH:
        return "mapped frame differs from expected provenance";
    case ADDRESS_SPACE_TABLE_STATUS_FRAME_IN_USE:
        return "physical frame is already mapped";
    case ADDRESS_SPACE_TABLE_STATUS_CORRUPTED:
        return "table state is corrupted";
    default:
        return "unknown table status";
    }
}

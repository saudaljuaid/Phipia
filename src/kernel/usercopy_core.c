/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usercopy_core.h"

#define USERCOPY_PERMISSION_MASK UINT32_C(15)

static void validation_clear(struct usercopy_validation *validation)
{
    validation->first_page = 0U;
    validation->last_page = 0U;
    validation->failed_page = 0U;
    validation->page_count = 0U;
    validation->crosses_page = false;
}

static bool mapping_metadata_valid(
    const struct address_space_mapping_snapshot *mapping
)
{
    const uint32_t permissions = mapping->permissions;

    if (mapping->virtual_page >
        (USERCOPY_USER_ADDRESS_MAX / USERCOPY_PAGE_SIZE) ||
        (permissions & ~USERCOPY_PERMISSION_MASK) != 0U ||
        ((permissions & ADDRESS_SPACE_PERMISSION_WRITE) != 0U &&
         (permissions & ADDRESS_SPACE_PERMISSION_EXECUTE) != 0U)) {
        return false;
    }
    return mapping->user_accessible ==
            ((permissions & ADDRESS_SPACE_PERMISSION_USER) != 0U) &&
        mapping->writable ==
            ((permissions & ADDRESS_SPACE_PERMISSION_WRITE) != 0U) &&
        mapping->executable ==
            ((permissions & ADDRESS_SPACE_PERMISSION_EXECUTE) != 0U);
}

enum usercopy_status usercopy_core_validate(
    uint64_t user_address,
    uint64_t length,
    enum usercopy_access access,
    const struct address_space_mapping_snapshot *mappings,
    size_t mapping_count,
    struct usercopy_validation *validation
)
{
    uint64_t last_address;
    uint64_t first_page;
    uint64_t last_page;
    uint64_t page_count;

    if (validation == NULL || (mapping_count != 0U && mappings == NULL)) {
        return USERCOPY_STATUS_NULL_ARGUMENT;
    }
    validation_clear(validation);
    if (access != USERCOPY_ACCESS_READ_FROM_USER &&
        access != USERCOPY_ACCESS_WRITE_TO_USER) {
        return USERCOPY_STATUS_INVALID_ACCESS;
    }
    if (length == 0U) {
        return USERCOPY_STATUS_OK;
    }
    if (length > USERCOPY_LENGTH_LIMIT) {
        return USERCOPY_STATUS_RANGE_TOO_LARGE;
    }
    if (length - 1U > UINT64_MAX - user_address) {
        return USERCOPY_STATUS_ADDRESS_OVERFLOW;
    }
    if (user_address < UINT64_C(4096)) {
        return USERCOPY_STATUS_NULL_PAGE;
    }
    if (user_address > USERCOPY_USER_ADDRESS_MAX) {
        return USERCOPY_STATUS_NONCANONICAL_ADDRESS;
    }
    last_address = user_address + length - 1U;
    if (last_address > USERCOPY_USER_ADDRESS_MAX) {
        return USERCOPY_STATUS_NONCANONICAL_ADDRESS;
    }

    first_page = user_address / USERCOPY_PAGE_SIZE;
    last_page = last_address / USERCOPY_PAGE_SIZE;
    page_count = last_page - first_page + 1U;
    validation->first_page = first_page;
    validation->last_page = last_page;
    validation->crosses_page = first_page != last_page;
    if (page_count > USERCOPY_PAGE_LIMIT) {
        return USERCOPY_STATUS_CROSS_PAGE_LIMIT;
    }
    validation->page_count = (uint32_t)page_count;
    if (mapping_count > USERCOPY_MAPPING_LIMIT) {
        return USERCOPY_STATUS_MAPPING_LIMIT;
    }

    for (size_t index = 0U; index < mapping_count; ++index) {
        if (!mapping_metadata_valid(&mappings[index])) {
            validation->failed_page = mappings[index].virtual_page;
            return USERCOPY_STATUS_INVALID_MAPPING;
        }
        for (size_t other = 0U; other < index; ++other) {
            if (mappings[other].virtual_page ==
                mappings[index].virtual_page) {
                validation->failed_page = mappings[index].virtual_page;
                return USERCOPY_STATUS_DUPLICATE_MAPPING;
            }
        }
    }

    for (uint64_t page = first_page; page <= last_page; ++page) {
        const struct address_space_mapping_snapshot *mapping = NULL;

        for (size_t index = 0U; index < mapping_count; ++index) {
            if (mappings[index].virtual_page == page) {
                mapping = &mappings[index];
                break;
            }
        }
        if (mapping == NULL) {
            validation->failed_page = page;
            return USERCOPY_STATUS_MAPPING_MISSING;
        }
        if (!mapping->user_accessible) {
            validation->failed_page = page;
            return USERCOPY_STATUS_SUPERVISOR_DENIED;
        }
        if ((mapping->permissions & ADDRESS_SPACE_PERMISSION_READ) == 0U) {
            validation->failed_page = page;
            return USERCOPY_STATUS_READ_DENIED;
        }
        if (access == USERCOPY_ACCESS_WRITE_TO_USER &&
            !mapping->writable) {
            validation->failed_page = page;
            return USERCOPY_STATUS_WRITE_DENIED;
        }
    }
    return USERCOPY_STATUS_OK;
}

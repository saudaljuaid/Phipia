/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_USERCOPY_H
#define ZENITH_USERCOPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/address_space.h>

#define USERCOPY_PAGE_SIZE UINT64_C(4096)
#define USERCOPY_PAGE_LIMIT UINT32_C(16)
#define USERCOPY_MAPPING_LIMIT ADDRESS_SPACE_PAGE_LIMIT
/* The byte and page bounds are both binding constraints. Exactly
 * USERCOPY_LENGTH_LIMIT bytes span sixteen pages only when page-aligned; the
 * same length from an unaligned address spans seventeen and must return
 * USERCOPY_STATUS_CROSS_PAGE_LIMIT. Runtime bindings must preserve both
 * checks and must size page metadata from the validated page count, never
 * from the byte limit alone. */
#define USERCOPY_LENGTH_LIMIT \
    (USERCOPY_PAGE_SIZE * (uint64_t)USERCOPY_PAGE_LIMIT)
#define USERCOPY_USER_ADDRESS_MAX UINT64_C(0x00007FFFFFFFFFFF)

enum usercopy_access {
    USERCOPY_ACCESS_READ_FROM_USER = 0,
    USERCOPY_ACCESS_WRITE_TO_USER
};

enum usercopy_status {
    USERCOPY_STATUS_OK = 0,
    USERCOPY_STATUS_NULL_ARGUMENT,
    USERCOPY_STATUS_INVALID_ACCESS,
    USERCOPY_STATUS_NULL_PAGE,
    USERCOPY_STATUS_NONCANONICAL_ADDRESS,
    USERCOPY_STATUS_ADDRESS_OVERFLOW,
    USERCOPY_STATUS_RANGE_TOO_LARGE,
    USERCOPY_STATUS_CROSS_PAGE_LIMIT,
    USERCOPY_STATUS_MAPPING_LIMIT,
    USERCOPY_STATUS_INVALID_MAPPING,
    USERCOPY_STATUS_DUPLICATE_MAPPING,
    USERCOPY_STATUS_MAPPING_MISSING,
    USERCOPY_STATUS_SUPERVISOR_DENIED,
    USERCOPY_STATUS_READ_DENIED,
    USERCOPY_STATUS_WRITE_DENIED
};

struct usercopy_validation {
    uint64_t first_page;
    uint64_t last_page;
    uint64_t failed_page;
    uint32_t page_count;
    bool crosses_page;
};

#endif

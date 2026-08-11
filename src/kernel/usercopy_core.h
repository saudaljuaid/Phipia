/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_USERCOPY_CORE_H
#define ZENITH_USERCOPY_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <zenith/usercopy.h>

/* Validates metadata only. It never dereferences the supplied user range. */
enum usercopy_status usercopy_core_validate(
    uint64_t user_address,
    uint64_t length,
    enum usercopy_access access,
    const struct address_space_mapping_snapshot *mappings,
    size_t mapping_count,
    struct usercopy_validation *validation
);

#endif

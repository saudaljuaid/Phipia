/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_ACPI_H
#define ZENITH_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/boot.h>

enum acpi_root_kind {
    ACPI_ROOT_NONE = 0,
    ACPI_ROOT_RSDT,
    ACPI_ROOT_XSDT
};

enum acpi_status {
    ACPI_STATUS_OK = 0,
    ACPI_STATUS_NULL_ARGUMENT,
    ACPI_STATUS_MISSING_RSDP,
    ACPI_STATUS_BAD_TAG_SIZE,
    ACPI_STATUS_BAD_SIGNATURE,
    ACPI_STATUS_BAD_LEGACY_CHECKSUM,
    ACPI_STATUS_BAD_REVISION,
    ACPI_STATUS_BAD_LENGTH,
    ACPI_STATUS_BAD_EXTENDED_CHECKSUM,
    ACPI_STATUS_NULL_ROOT,
    ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP
};

struct acpi_root {
    enum acpi_root_kind kind;
    uint8_t revision;
    char oem_id[7];
    uint64_t physical_address;
};

enum acpi_status acpi_root_discover(
    const struct boot_context *context,
    struct acpi_root *root
);
bool acpi_self_test(void);
const char *acpi_root_kind_string(enum acpi_root_kind kind);
const char *acpi_status_string(enum acpi_status status);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_ACPI_H
#define SENERI_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/boot.h>

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
    ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP,
    ACPI_STATUS_BAD_ROOT_SIGNATURE,
    ACPI_STATUS_BAD_ROOT_LENGTH,
    ACPI_STATUS_BAD_ROOT_ENTRY_SIZE,
    ACPI_STATUS_TOO_MANY_ROOT_ENTRIES,
    ACPI_STATUS_BAD_ROOT_CHECKSUM,
    ACPI_STATUS_NULL_TABLE,
    ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP,
    ACPI_STATUS_BAD_TABLE_LENGTH,
    ACPI_STATUS_BAD_TABLE_CHECKSUM,
    ACPI_STATUS_MISSING_MADT,
    ACPI_STATUS_DUPLICATE_MADT,
    ACPI_STATUS_BAD_MADT_LENGTH,
    ACPI_STATUS_BAD_MADT_FLAGS
};

struct acpi_root {
    enum acpi_root_kind kind;
    uint8_t revision;
    char oem_id[7];
    uint64_t physical_address;
};

struct acpi_madt {
    uint64_t physical_address;
    uint32_t length;
    uint32_t local_apic_address;
    uint32_t flags;
    size_t root_entry_count;
    uint8_t revision;
    char oem_id[7];
    char oem_table_id[9];
};

enum acpi_status acpi_root_discover(
    const struct boot_context *context,
    struct acpi_root *root
);
enum acpi_status acpi_madt_discover(
    const struct acpi_root *root,
    struct acpi_madt *madt
);
bool acpi_self_test(void);
bool acpi_tables_self_test(void);
const char *acpi_root_kind_string(enum acpi_root_kind kind);
const char *acpi_status_string(enum acpi_status status);

#endif

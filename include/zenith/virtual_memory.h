/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_VIRTUAL_MEMORY_H
#define ZENITH_VIRTUAL_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>

#define VIRTUAL_MEMORY_DEVICE_WINDOW_BASE UINT64_C(0xFFFF800000000000)

enum virtual_memory_status {
    VIRTUAL_MEMORY_STATUS_OK = 0,
    VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT,
    VIRTUAL_MEMORY_STATUS_ALREADY_INITIALIZED,
    VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED,
    VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED,
    VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU,
    VIRTUAL_MEMORY_STATUS_BAD_KERNEL_LAYOUT,
    VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT,
    VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS,
    VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE,
    VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW,
    VIRTUAL_MEMORY_STATUS_RANGE_TOO_LARGE,
    VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS,
    VIRTUAL_MEMORY_STATUS_TABLE_LIMIT,
    VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT,
    VIRTUAL_MEMORY_STATUS_NOT_MAPPED,
    VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS,
    VIRTUAL_MEMORY_STATUS_TOO_MANY_DEVICES,
    VIRTUAL_MEMORY_STATUS_PAT_FAILURE,
    VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE
};

enum virtual_memory_permissions {
    VIRTUAL_MEMORY_READ_ONLY = 0,
    VIRTUAL_MEMORY_WRITABLE = 1U << 0,
    VIRTUAL_MEMORY_EXECUTABLE = 1U << 1,
    VIRTUAL_MEMORY_DEVICE = 1U << 2
};

struct virtual_memory_mapping {
    uint64_t physical_address;
    uint32_t permissions;
};

struct virtual_memory_layout {
    uint64_t root_table_address;
    uint64_t local_apic_virtual_address;
    uint64_t io_apic_virtual_addresses[ACPI_MAX_IO_APICS];
    size_t io_apic_count;
    size_t table_page_count;
};

bool virtual_memory_self_test(void);
enum virtual_memory_status virtual_memory_initialize(
    const struct acpi_topology *topology,
    struct virtual_memory_layout *layout
);
enum virtual_memory_status virtual_memory_validate(void);
enum virtual_memory_status virtual_memory_query(
    uint64_t virtual_address,
    struct virtual_memory_mapping *mapping
);
bool virtual_memory_ready(void);
const char *virtual_memory_status_string(enum virtual_memory_status status);

#endif

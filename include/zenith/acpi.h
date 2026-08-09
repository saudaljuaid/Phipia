/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_ACPI_H
#define ZENITH_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>

#define ACPI_MAX_MADT_ENTRIES 1024U
#define ACPI_MAX_PROCESSORS 256U
#define ACPI_MAX_IO_APICS 16U
#define ACPI_MAX_INTERRUPT_OVERRIDES 16U

#define ACPI_PROCESSOR_ENABLED UINT32_C(1)
#define ACPI_PROCESSOR_ONLINE_CAPABLE UINT32_C(2)

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
    ACPI_STATUS_BAD_MADT_FLAGS,
    ACPI_STATUS_BAD_MADT_ENTRY_HEADER,
    ACPI_STATUS_BAD_MADT_ENTRY_LENGTH,
    ACPI_STATUS_TOO_MANY_MADT_ENTRIES,
    ACPI_STATUS_BAD_PROCESSOR_FLAGS,
    ACPI_STATUS_BAD_PROCESSOR_RESERVED,
    ACPI_STATUS_TOO_MANY_PROCESSORS,
    ACPI_STATUS_DUPLICATE_PROCESSOR_UID,
    ACPI_STATUS_DUPLICATE_APIC_ID,
    ACPI_STATUS_BAD_IO_APIC_RESERVED,
    ACPI_STATUS_NULL_IO_APIC_ADDRESS,
    ACPI_STATUS_TOO_MANY_IO_APICS,
    ACPI_STATUS_DUPLICATE_IO_APIC_ID,
    ACPI_STATUS_DUPLICATE_IO_APIC_ADDRESS,
    ACPI_STATUS_DUPLICATE_GSI_BASE,
    ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_BUS,
    ACPI_STATUS_BAD_INTERRUPT_OVERRIDE_SOURCE,
    ACPI_STATUS_BAD_INTERRUPT_FLAGS,
    ACPI_STATUS_TOO_MANY_INTERRUPT_OVERRIDES,
    ACPI_STATUS_DUPLICATE_INTERRUPT_SOURCE,
    ACPI_STATUS_DUPLICATE_INTERRUPT_GSI,
    ACPI_STATUS_BAD_LOCAL_APIC_OVERRIDE_RESERVED,
    ACPI_STATUS_NULL_LOCAL_APIC_ADDRESS,
    ACPI_STATUS_DUPLICATE_LOCAL_APIC_OVERRIDE,
    ACPI_STATUS_MISSING_USABLE_PROCESSOR,
    ACPI_STATUS_MISSING_IO_APIC
};

enum acpi_processor_kind {
    ACPI_PROCESSOR_LOCAL_APIC = 0,
    ACPI_PROCESSOR_LOCAL_X2APIC
};

enum acpi_interrupt_polarity {
    ACPI_POLARITY_CONFORMS = 0,
    ACPI_POLARITY_ACTIVE_HIGH = 1,
    ACPI_POLARITY_ACTIVE_LOW = 3
};

enum acpi_interrupt_trigger {
    ACPI_TRIGGER_CONFORMS = 0,
    ACPI_TRIGGER_EDGE = 1,
    ACPI_TRIGGER_LEVEL = 3
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

struct acpi_processor {
    enum acpi_processor_kind kind;
    uint32_t acpi_uid;
    uint32_t apic_id;
    uint32_t flags;
};

struct acpi_io_apic {
    uint32_t id;
    uint32_t physical_address;
    uint32_t global_interrupt_base;
};

struct acpi_interrupt_override {
    uint32_t source;
    uint32_t global_interrupt;
    enum acpi_interrupt_polarity polarity;
    enum acpi_interrupt_trigger trigger;
};

struct acpi_topology {
    uint64_t local_apic_address;
    size_t madt_entry_count;
    size_t ignored_entry_count;
    size_t ignored_processor_count;
    size_t processor_count;
    size_t enabled_processor_count;
    size_t online_capable_processor_count;
    size_t io_apic_count;
    size_t interrupt_override_count;
    bool local_apic_address_overridden;
    struct acpi_processor processors[ACPI_MAX_PROCESSORS];
    struct acpi_io_apic io_apics[ACPI_MAX_IO_APICS];
    struct acpi_interrupt_override
        interrupt_overrides[ACPI_MAX_INTERRUPT_OVERRIDES];
};

enum acpi_status acpi_root_discover(
    const struct boot_context *context,
    struct acpi_root *root
);
enum acpi_status acpi_madt_discover(
    const struct acpi_root *root,
    struct acpi_madt *madt
);
enum acpi_status acpi_topology_discover(
    const struct acpi_madt *madt,
    struct acpi_topology *topology
);
bool acpi_self_test(void);
bool acpi_tables_self_test(void);
bool acpi_topology_self_test(void);
const char *acpi_root_kind_string(enum acpi_root_kind kind);
const char *acpi_status_string(enum acpi_status status);

#endif

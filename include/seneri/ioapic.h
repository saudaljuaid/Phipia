/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_IOAPIC_H
#define SENERI_IOAPIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>

enum ioapic_status {
    IOAPIC_STATUS_OK = 0,
    IOAPIC_STATUS_NULL_ARGUMENT,
    IOAPIC_STATUS_ALREADY_INITIALIZED,
    IOAPIC_STATUS_NOT_INITIALIZED,
    IOAPIC_STATUS_INTERRUPTS_ENABLED,
    IOAPIC_STATUS_MISSING_IO_APIC,
    IOAPIC_STATUS_ID_DISAGREES_WITH_ACPI,
    IOAPIC_STATUS_TOO_FEW_ENTRIES,
    IOAPIC_STATUS_OVERLAPPING_INTERRUPT_BASE,
    IOAPIC_STATUS_BAD_IRQ,
    IOAPIC_STATUS_BAD_VECTOR,
    IOAPIC_STATUS_UNROUTABLE_INTERRUPT,
    IOAPIC_STATUS_LEVEL_TRIGGERED,
    IOAPIC_STATUS_READBACK_MISMATCH
};

struct ioapic_unit {
    uint32_t identifier;
    uint32_t address;
    uint32_t interrupt_base;
    uint8_t version;
    uint8_t entry_count;
};

struct ioapic_state {
    size_t count;
    struct ioapic_unit units[ACPI_MAX_IO_APICS];
};

enum ioapic_status ioapic_initialize(const struct acpi_topology *topology);
enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    uint32_t destination
);
enum ioapic_status ioapic_mask_isa_irq(uint8_t irq);
struct ioapic_state ioapic_get_state(void);
bool ioapic_is_initialized(void);
bool ioapic_self_test(void);
const char *ioapic_status_string(enum ioapic_status status);

#endif

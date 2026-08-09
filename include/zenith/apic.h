/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_APIC_H
#define ZENITH_APIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>
#include <zenith/virtual_memory.h>

#define APIC_TIMER_VECTOR UINT8_C(0x20)
#define APIC_SPURIOUS_VECTOR UINT8_C(0xFF)

enum apic_status {
    APIC_STATUS_OK = 0,
    APIC_STATUS_NULL_ARGUMENT,
    APIC_STATUS_ALREADY_INITIALIZED,
    APIC_STATUS_NOT_INITIALIZED,
    APIC_STATUS_INTERRUPTS_ENABLED,
    APIC_STATUS_INTERRUPT_FAILURE,
    APIC_STATUS_VIRTUAL_MEMORY_FAILURE,
    APIC_STATUS_UNSUPPORTED_CPU,
    APIC_STATUS_X2APIC_ACTIVE,
    APIC_STATUS_LOCAL_APIC_DISABLED,
    APIC_STATUS_NOT_BOOTSTRAP_PROCESSOR,
    APIC_STATUS_LOCAL_APIC_BASE_MISMATCH,
    APIC_STATUS_BAD_LOCAL_APIC_VERSION,
    APIC_STATUS_LOCAL_APIC_ID_MISMATCH,
    APIC_STATUS_BAD_IO_APIC_COUNT,
    APIC_STATUS_BAD_IO_APIC_ADDRESS,
    APIC_STATUS_BAD_IO_APIC_ID,
    APIC_STATUS_BAD_IO_APIC_VERSION,
    APIC_STATUS_BAD_GSI_RANGE,
    APIC_STATUS_OVERLAPPING_GSI_RANGE,
    APIC_STATUS_BAD_TIMER_OVERRIDE,
    APIC_STATUS_TIMER_GSI_UNROUTABLE,
    APIC_STATUS_PIC_NOT_MASKED,
    APIC_STATUS_REGISTER_MISMATCH
};

struct apic_io_controller {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint32_t id;
    uint32_t version;
    uint32_t global_interrupt_base;
    uint32_t redirection_entry_count;
};

struct apic_configuration {
    uint64_t local_apic_physical_address;
    uint32_t local_apic_id;
    uint32_t local_apic_version;
    uint32_t local_apic_max_lvt;
    size_t io_apic_count;
    struct apic_io_controller io_apics[ACPI_MAX_IO_APICS];
    uint32_t timer_global_interrupt;
    uint32_t timer_input;
    size_t timer_io_apic_index;
    bool timer_active_low;
    bool timer_level_triggered;
};

bool apic_self_test(void);
enum apic_status apic_initialize(
    const struct acpi_topology *topology,
    const struct virtual_memory_layout *layout,
    struct apic_configuration *configuration
);
enum apic_status apic_validate(void);
enum apic_status apic_timer_set_mask(bool masked);
bool apic_timer_is_masked(void);
bool apic_vector_requires_eoi(uint8_t vector);
void apic_send_eoi(void);
uint64_t apic_eoi_count(void);
bool apic_spurious_self_test(void);
bool apic_ready(void);
const char *apic_status_string(enum apic_status status);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_APIC_H
#define SENERI_APIC_H

#include <stdbool.h>
#include <stdint.h>

#include <seneri/acpi.h>

/*
 * Intel SDM volume 3A section 11.9 requires the spurious vector's low four
 * bits to be set on some processor generations, so Seneri fixes it at 0xFF.
 */
#define APIC_SPURIOUS_VECTOR UINT8_C(0xFF)

enum apic_status {
    APIC_STATUS_OK = 0,
    APIC_STATUS_NULL_ARGUMENT,
    APIC_STATUS_ALREADY_ONLINE,
    APIC_STATUS_INTERRUPTS_ENABLED,
    APIC_STATUS_UNSUPPORTED,
    APIC_STATUS_HARDWARE_DISABLED,
    APIC_STATUS_X2APIC_MODE,
    APIC_STATUS_NOT_BOOTSTRAP,
    APIC_STATUS_NULL_BASE,
    APIC_STATUS_BASE_OUTSIDE_EARLY_MAP,
    APIC_STATUS_BASE_DISAGREES_WITH_ACPI,
    APIC_STATUS_EXTERNAL_APIC,
    APIC_STATUS_TOO_FEW_LVT_ENTRIES,
    APIC_STATUS_ID_DISAGREES_WITH_ACPI,
    APIC_STATUS_INTERRUPT_FAILURE,
    APIC_STATUS_READBACK_MISMATCH,
    APIC_STATUS_NOT_ONLINE
};

struct apic_state {
    uint64_t base_address;
    uint32_t id;
    uint8_t version;
    uint8_t max_lvt_entry;
    bool legacy_interrupts_routed;
    bool online;
};

enum apic_status apic_bring_online(const struct acpi_topology *topology);
enum apic_status apic_retire_legacy_routing(void);
void apic_send_eoi(void);
struct apic_state apic_get_state(void);
bool apic_is_online(void);
uint64_t apic_spurious_count(void);
bool apic_self_test(void);
const char *apic_status_string(enum apic_status status);

#endif

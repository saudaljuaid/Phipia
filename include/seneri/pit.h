/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_PIT_H
#define SENERI_PIT_H

#include <stdbool.h>
#include <stdint.h>

enum pit_status {
    PIT_STATUS_OK = 0,
    PIT_STATUS_ALREADY_RUNNING,
    PIT_STATUS_NOT_RUNNING,
    PIT_STATUS_INTERRUPTS_ENABLED,
    PIT_STATUS_BAD_FREQUENCY,
    PIT_STATUS_INTERRUPT_FAILURE,
    PIT_STATUS_PIC_FAILURE,
    PIT_STATUS_BAD_ROUTE,
    PIT_STATUS_IOAPIC_FAILURE,
    PIT_STATUS_RETIRED
};

/*
 * The same timer hardware reaches the processor by two different paths. Which
 * one is in use is the caller's decision, not a hidden default.
 */
enum pit_route {
    PIT_ROUTE_LEGACY_PIC = 0,
    PIT_ROUTE_IO_APIC
};

enum pit_status pit_start(uint32_t frequency_hz, enum pit_route route);
enum pit_route pit_active_route(void);
enum pit_status pit_stop(void);

/*
 * Take the 8254 off the machine for good. Nothing measures time against it any
 * more - the ACPI power management timer does that, and both derived clocks are
 * calibrated from it - so the counter is stopped, its redirection entry masked,
 * and the subsystem latched shut. Every later mutation is refused rather than
 * quietly re-arming a timer the kernel no longer reasons about.
 */
enum pit_status pit_retire(void);
bool pit_is_retired(void);
uint64_t pit_ticks(void);
uint32_t pit_frequency(void);
bool pit_is_running(void);
enum pit_status pit_wait_for_ticks(uint64_t tick_count);
const char *pit_status_string(enum pit_status status);

#endif

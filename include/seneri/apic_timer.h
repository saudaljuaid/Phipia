/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_APIC_TIMER_H
#define SENERI_APIC_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <seneri/interrupts.h>

#define APIC_TIMER_VECTOR ((uint8_t)INTERRUPT_LOCAL_APIC_BASE)

enum apic_timer_status {
    APIC_TIMER_STATUS_OK = 0,
    APIC_TIMER_STATUS_APIC_OFFLINE,
    APIC_TIMER_STATUS_INTERRUPTS_ENABLED,
    APIC_TIMER_STATUS_ALREADY_CALIBRATED,
    APIC_TIMER_STATUS_NOT_CALIBRATED,
    APIC_TIMER_STATUS_ALREADY_RUNNING,
    APIC_TIMER_STATUS_NOT_RUNNING,
    APIC_TIMER_STATUS_REFERENCE_FAILURE,
    APIC_TIMER_STATUS_NOT_COUNTING,
    APIC_TIMER_STATUS_COUNTER_EXHAUSTED,
    APIC_TIMER_STATUS_BAD_FREQUENCY,
    APIC_TIMER_STATUS_INTERRUPT_FAILURE,
    APIC_TIMER_STATUS_READBACK_MISMATCH
};

enum apic_timer_status apic_timer_calibrate(void);
enum apic_timer_status apic_timer_start(uint32_t frequency_hz);
enum apic_timer_status apic_timer_stop(void);
enum apic_timer_status apic_timer_wait_for_ticks(uint64_t tick_count);
uint64_t apic_timer_ticks(void);
uint64_t apic_timer_counts_per_second(void);
bool apic_timer_is_calibrated(void);
bool apic_timer_is_running(void);
bool apic_timer_self_test(void);
const char *apic_timer_status_string(enum apic_timer_status status);

#endif

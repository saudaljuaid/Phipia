/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_TIME_H
#define ZENITH_TIME_H

#include <stdbool.h>
#include <stdint.h>

enum kernel_time_status {
    KERNEL_TIME_STATUS_OK = 0,
    KERNEL_TIME_STATUS_NULL_ARGUMENT,
    KERNEL_TIME_STATUS_ALREADY_INITIALIZED,
    KERNEL_TIME_STATUS_NOT_INITIALIZED,
    KERNEL_TIME_STATUS_INTERRUPTS_ENABLED,
    KERNEL_TIME_STATUS_APIC_FAILURE,
    KERNEL_TIME_STATUS_BAD_TIMER_DIVIDE,
    KERNEL_TIME_STATUS_BAD_TIMER_FREQUENCY,
    KERNEL_TIME_STATUS_BAD_TIMER_SAMPLE,
    KERNEL_TIME_STATUS_OVERFLOW,
    KERNEL_TIME_STATUS_TIMEOUT,
    KERNEL_TIME_STATUS_VALIDATION_FAILURE,
    KERNEL_TIME_STATUS_ROLLBACK_FAILURE
};

bool kernel_time_self_test(void);
enum kernel_time_status kernel_time_initialize(uint32_t frequency_hz);
enum kernel_time_status kernel_time_validate(void);
uint64_t kernel_time_monotonic_ticks(void);
uint32_t kernel_time_frequency_hz(void);
uint64_t kernel_time_period_nanoseconds(void);
enum kernel_time_status kernel_time_ticks_to_nanoseconds(
    uint64_t ticks,
    uint64_t *nanoseconds
);
enum kernel_time_status kernel_time_monotonic_nanoseconds(
    uint64_t *nanoseconds
);
enum kernel_time_status kernel_time_wait_for_ticks(uint64_t tick_count);
const char *kernel_time_status_string(enum kernel_time_status status);

#endif

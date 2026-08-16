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
    PIT_STATUS_PIC_FAILURE
};

enum pit_status pit_start(uint32_t frequency_hz);
enum pit_status pit_stop(void);
uint64_t pit_ticks(void);
uint32_t pit_frequency(void);
bool pit_is_running(void);
enum pit_status pit_wait_for_ticks(uint64_t tick_count);
const char *pit_status_string(enum pit_status status);

#endif

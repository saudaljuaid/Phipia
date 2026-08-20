/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PYRENIS_CLOCK_H
#define PYRENIS_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * One place that answers "how long since boot". Every clock before this one
 * handed callers a raw counter sample and left them to remember which counter it
 * came from, how wide it was, and what its rate meant. Nothing that sleeps or
 * schedules can be written against that.
 */

enum clock_source {
    CLOCK_SOURCE_NONE = 0,
    CLOCK_SOURCE_TSC
};

enum clock_status {
    CLOCK_STATUS_OK = 0,
    CLOCK_STATUS_NOT_STARTED,
    CLOCK_STATUS_ALREADY_STARTED,
    CLOCK_STATUS_NO_SOURCE
};

struct clock_state {
    enum clock_source source;
    uint64_t origin;
    uint64_t last_reported_ns;
    uint64_t backward_steps;
    bool started;
};

/*
 * Fix the origin of the clock. Requires a calibrated time-stamp counter, whose
 * rate is itself derived from the ACPI power management timer; see
 * docs/MONOTONIC_TIME.md for why that counter is the source rather than the ACPI
 * timer directly.
 */
enum clock_status clock_start(void);

/*
 * Nanoseconds since clock_start. Returns zero before it, which is the same value
 * the instant after it, because there is no honest duration to report either way.
 */
uint64_t clock_monotonic_ns(void);

struct clock_state clock_get_state(void);
bool clock_is_started(void);
const char *clock_source_string(enum clock_source source);
bool clock_self_test(void);
const char *clock_status_string(enum clock_status status);

#endif

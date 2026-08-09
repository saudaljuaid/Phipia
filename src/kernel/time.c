/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/time.h>

#define KERNEL_TIME_WAIT_ATTEMPT_LIMIT UINT64_C(100000000)

static volatile uint64_t monotonic_tick_count __attribute__((aligned(8)));
static struct apic_local_timer_configuration timer_configuration;
static bool tick_counter_overflowed;
static bool invalid_interrupt_observed;
static bool initialized;

static enum kernel_time_status time_status_from_apic(enum apic_status status)
{
    switch (status) {
    case APIC_STATUS_OK:
        return KERNEL_TIME_STATUS_OK;
    case APIC_STATUS_BAD_TIMER_DIVIDE:
        return KERNEL_TIME_STATUS_BAD_TIMER_DIVIDE;
    case APIC_STATUS_BAD_TIMER_FREQUENCY:
        return KERNEL_TIME_STATUS_BAD_TIMER_FREQUENCY;
    case APIC_STATUS_BAD_TIMER_SAMPLE:
        return KERNEL_TIME_STATUS_BAD_TIMER_SAMPLE;
    case APIC_STATUS_TIMER_TIMEOUT:
        return KERNEL_TIME_STATUS_TIMEOUT;
    case APIC_STATUS_REGISTER_MISMATCH:
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    default:
        return KERNEL_TIME_STATUS_APIC_FAILURE;
    }
}

static void timer_configuration_reset(void)
{
    uint8_t *bytes = (uint8_t *)(void *)&timer_configuration;

    for (size_t index = 0U; index < sizeof(timer_configuration); ++index) {
        bytes[index] = 0U;
    }
}

static void timer_configuration_copy(
    const struct apic_local_timer_configuration *source
)
{
    timer_configuration.divide_configuration = source->divide_configuration;
    timer_configuration.divisor = source->divisor;
    timer_configuration.calibrated_frequency_hz =
        source->calibrated_frequency_hz;
    timer_configuration.periodic_initial_count =
        source->periodic_initial_count;
    timer_configuration.requested_frequency_hz =
        source->requested_frequency_hz;
    timer_configuration.periodic_frequency_hz =
        source->periodic_frequency_hz;
    timer_configuration.minimum_sample_hz = source->minimum_sample_hz;
    timer_configuration.maximum_sample_hz = source->maximum_sample_hz;
    timer_configuration.period_nanoseconds = source->period_nanoseconds;
}

static void tick_increment(uint64_t *ticks, bool *overflowed)
{
    if (*ticks == UINT64_MAX) {
        *overflowed = true;
        return;
    }

    ++*ticks;
}

static enum kernel_time_status ticks_to_nanoseconds_with_period(
    uint64_t ticks,
    uint64_t period_nanoseconds,
    uint64_t *nanoseconds
)
{
    if (nanoseconds == NULL) {
        return KERNEL_TIME_STATUS_NULL_ARGUMENT;
    }

    *nanoseconds = 0U;

    if (period_nanoseconds == 0U) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    if (ticks != 0U && period_nanoseconds > UINT64_MAX / ticks) {
        return KERNEL_TIME_STATUS_OVERFLOW;
    }

    *nanoseconds = ticks * period_nanoseconds;
    return KERNEL_TIME_STATUS_OK;
}

static void local_timer_handler(
    struct interrupt_frame *frame,
    void *context
)
{
    uint64_t ticks;
    bool overflowed;

    (void)context;

    if (frame == NULL || frame->vector != APIC_LOCAL_TIMER_VECTOR) {
        invalid_interrupt_observed = true;
        return;
    }

    ticks = monotonic_tick_count;
    overflowed = tick_counter_overflowed;
    tick_increment(&ticks, &overflowed);
    monotonic_tick_count = ticks;
    tick_counter_overflowed = overflowed;
}

bool kernel_time_self_test(void)
{
    uint64_t nanoseconds;
    uint64_t ticks = UINT64_MAX - 1U;
    bool overflowed = false;
    const uint64_t period = UINT64_C(1000000);
    const uint64_t largest_convertible = UINT64_MAX / period;

    if (initialized ||
        time_status_from_apic(APIC_STATUS_OK) != KERNEL_TIME_STATUS_OK ||
        time_status_from_apic(APIC_STATUS_BAD_TIMER_DIVIDE) !=
            KERNEL_TIME_STATUS_BAD_TIMER_DIVIDE ||
        time_status_from_apic(APIC_STATUS_BAD_TIMER_FREQUENCY) !=
            KERNEL_TIME_STATUS_BAD_TIMER_FREQUENCY ||
        time_status_from_apic(APIC_STATUS_BAD_TIMER_SAMPLE) !=
            KERNEL_TIME_STATUS_BAD_TIMER_SAMPLE ||
        time_status_from_apic(APIC_STATUS_TIMER_TIMEOUT) !=
            KERNEL_TIME_STATUS_TIMEOUT ||
        time_status_from_apic(APIC_STATUS_REGISTER_MISMATCH) !=
            KERNEL_TIME_STATUS_VALIDATION_FAILURE ||
        time_status_from_apic(APIC_STATUS_PIT_FAILURE) !=
            KERNEL_TIME_STATUS_APIC_FAILURE ||
        ticks_to_nanoseconds_with_period(0U, period, &nanoseconds) !=
            KERNEL_TIME_STATUS_OK ||
        nanoseconds != 0U ||
        ticks_to_nanoseconds_with_period(1U, period, &nanoseconds) !=
            KERNEL_TIME_STATUS_OK ||
        nanoseconds != period ||
        ticks_to_nanoseconds_with_period(
            largest_convertible,
            period,
            &nanoseconds
        ) != KERNEL_TIME_STATUS_OK ||
        nanoseconds != largest_convertible * period ||
        ticks_to_nanoseconds_with_period(
            largest_convertible + 1U,
            period,
            &nanoseconds
        ) != KERNEL_TIME_STATUS_OVERFLOW ||
        ticks_to_nanoseconds_with_period(1U, 0U, &nanoseconds) !=
            KERNEL_TIME_STATUS_VALIDATION_FAILURE ||
        ticks_to_nanoseconds_with_period(1U, UINT64_MAX, &nanoseconds) !=
            KERNEL_TIME_STATUS_OK ||
        nanoseconds != UINT64_MAX ||
        ticks_to_nanoseconds_with_period(1U, period, NULL) !=
            KERNEL_TIME_STATUS_NULL_ARGUMENT) {
        return false;
    }

    tick_increment(&ticks, &overflowed);

    if (ticks != UINT64_MAX || overflowed) {
        return false;
    }

    tick_increment(&ticks, &overflowed);
    return ticks == UINT64_MAX && overflowed;
}

enum kernel_time_status kernel_time_initialize(uint32_t frequency_hz)
{
    struct apic_local_timer_configuration candidate;
    enum apic_status apic_status;

    if (initialized) {
        return KERNEL_TIME_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return KERNEL_TIME_STATUS_INTERRUPTS_ENABLED;
    }

    monotonic_tick_count = 0U;
    tick_counter_overflowed = false;
    invalid_interrupt_observed = false;
    timer_configuration_reset();
    apic_status = apic_local_timer_calibrate_and_start(
        frequency_hz,
        local_timer_handler,
        NULL,
        &candidate
    );

    if (apic_status != APIC_STATUS_OK) {
        return time_status_from_apic(apic_status);
    }

    timer_configuration_copy(&candidate);
    initialized = true;

    if (kernel_time_validate() != KERNEL_TIME_STATUS_OK) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    return KERNEL_TIME_STATUS_OK;
}

enum kernel_time_status kernel_time_validate(void)
{
    if (!initialized) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return KERNEL_TIME_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_counter_overflowed) {
        return KERNEL_TIME_STATUS_OVERFLOW;
    }

    if (invalid_interrupt_observed || !apic_local_timer_active() ||
        apic_local_timer_validate() != APIC_STATUS_OK ||
        apic_validate() != APIC_STATUS_OK ||
        timer_configuration.requested_frequency_hz == 0U ||
        timer_configuration.periodic_frequency_hz == 0U ||
        timer_configuration.period_nanoseconds == 0U) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    return KERNEL_TIME_STATUS_OK;
}

uint64_t kernel_time_monotonic_ticks(void)
{
    return monotonic_tick_count;
}

uint32_t kernel_time_frequency_hz(void)
{
    return initialized ? timer_configuration.periodic_frequency_hz : 0U;
}

uint64_t kernel_time_period_nanoseconds(void)
{
    return initialized ? timer_configuration.period_nanoseconds : 0U;
}

enum kernel_time_status kernel_time_ticks_to_nanoseconds(
    uint64_t ticks,
    uint64_t *nanoseconds
)
{
    if (nanoseconds == NULL) {
        return KERNEL_TIME_STATUS_NULL_ARGUMENT;
    }

    if (!initialized) {
        *nanoseconds = 0U;
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    return ticks_to_nanoseconds_with_period(
        ticks,
        timer_configuration.period_nanoseconds,
        nanoseconds
    );
}

enum kernel_time_status kernel_time_monotonic_nanoseconds(
    uint64_t *nanoseconds
)
{
    if (nanoseconds == NULL) {
        return KERNEL_TIME_STATUS_NULL_ARGUMENT;
    }

    if (!initialized) {
        *nanoseconds = 0U;
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (tick_counter_overflowed) {
        *nanoseconds = 0U;
        return KERNEL_TIME_STATUS_OVERFLOW;
    }

    return kernel_time_ticks_to_nanoseconds(
        monotonic_tick_count,
        nanoseconds
    );
}

enum kernel_time_status kernel_time_wait_for_ticks(uint64_t tick_count)
{
    uint64_t target;

    if (!initialized) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return KERNEL_TIME_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_count == 0U) {
        return KERNEL_TIME_STATUS_OK;
    }

    if (tick_counter_overflowed ||
        tick_count > UINT64_MAX - monotonic_tick_count) {
        return KERNEL_TIME_STATUS_OVERFLOW;
    }

    target = monotonic_tick_count + tick_count;
    cpu_interrupt_enable();

    for (uint64_t attempt = 0U;
         attempt < KERNEL_TIME_WAIT_ATTEMPT_LIMIT;
         ++attempt) {
        if (monotonic_tick_count >= target) {
            cpu_interrupt_disable();

            if (tick_counter_overflowed) {
                return KERNEL_TIME_STATUS_OVERFLOW;
            }

            if (invalid_interrupt_observed) {
                return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
            }

            return KERNEL_TIME_STATUS_OK;
        }

        __asm__ volatile ("pause" : : : "memory");
    }

    cpu_interrupt_disable();
    return KERNEL_TIME_STATUS_TIMEOUT;
}

const char *kernel_time_status_string(enum kernel_time_status status)
{
    switch (status) {
    case KERNEL_TIME_STATUS_OK:
        return "ok";
    case KERNEL_TIME_STATUS_NULL_ARGUMENT:
        return "null monotonic-time argument";
    case KERNEL_TIME_STATUS_ALREADY_INITIALIZED:
        return "monotonic timebase was initialized twice";
    case KERNEL_TIME_STATUS_NOT_INITIALIZED:
        return "monotonic timebase is not initialized";
    case KERNEL_TIME_STATUS_INTERRUPTS_ENABLED:
        return "monotonic-time operation requires IF cleared";
    case KERNEL_TIME_STATUS_APIC_FAILURE:
        return "Local APIC timer calibration or activation failed";
    case KERNEL_TIME_STATUS_BAD_TIMER_DIVIDE:
        return "Local APIC timer divide value was rejected";
    case KERNEL_TIME_STATUS_BAD_TIMER_FREQUENCY:
        return "Local APIC timer calibration frequency was rejected";
    case KERNEL_TIME_STATUS_BAD_TIMER_SAMPLE:
        return "Local APIC timer calibration sample was rejected";
    case KERNEL_TIME_STATUS_OVERFLOW:
        return "monotonic-time arithmetic reached its explicit limit";
    case KERNEL_TIME_STATUS_TIMEOUT:
        return "monotonic timer wait exceeded its bounded attempt limit";
    case KERNEL_TIME_STATUS_VALIDATION_FAILURE:
        return "monotonic timebase validation failed";
    default:
        return "unknown monotonic-time status";
    }
}

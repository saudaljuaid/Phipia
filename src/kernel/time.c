/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/preempt.h>
#include <zenith/scheduler.h>
#include <zenith/spinlock.h>
#include <zenith/time.h>

#define KERNEL_TIME_WAIT_ATTEMPT_LIMIT UINT64_C(100000000)
#define KERNEL_TIME_LOCK_INDEX UINT32_C(0)

_Static_assert(SPINLOCK_CLASS_TIME < SPINLOCK_CLASS_SCHEDULER,
    "timer state must precede scheduler state in the lock order");

static volatile uint64_t monotonic_tick_count __attribute__((aligned(8)));
static struct apic_local_timer_configuration timer_configuration;
static bool tick_counter_overflowed;
static bool invalid_interrupt_observed;
static bool rollback_pending;
static bool initialized;
static bool time_poisoned;
static bool time_lock_registered;
static struct spinlock time_lock;

static enum kernel_time_status kernel_time_validate_unguarded(
    bool caller_interrupts_enabled
);

static bool time_lock_register(void)
{
    enum spinlock_status status;

    if (time_lock_registered) {
        return true;
    }

    status = spinlock_runtime_register(
        &time_lock,
        KERNEL_TIME_LOCK_INDEX,
        SPINLOCK_CLASS_TIME,
        UINT8_C(0)
    );
    if (status != SPINLOCK_STATUS_OK) {
        return false;
    }

    time_lock_registered = true;
    return true;
}

static bool time_lock_acquire(struct spinlock_irq_token *token)
{
    return time_lock_registered && !time_poisoned &&
        spinlock_irqsave_acquire(&time_lock, token) == SPINLOCK_STATUS_OK;
}

static enum kernel_time_status time_lock_failure_status(void)
{
    return time_lock_registered
        ? KERNEL_TIME_STATUS_VALIDATION_FAILURE
        : KERNEL_TIME_STATUS_NOT_INITIALIZED;
}

static bool time_lock_exit(struct spinlock_irq_token *token)
{
    if (spinlock_irqrestore_release(&time_lock, token) !=
            SPINLOCK_STATUS_OK) {
        time_poisoned = true;
        return false;
    }
    return true;
}

static enum kernel_time_status time_lock_finish(
    struct spinlock_irq_token *token,
    enum kernel_time_status status
)
{
    return time_lock_exit(token)
        ? status
        : KERNEL_TIME_STATUS_VALIDATION_FAILURE;
}

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

static void kernel_time_state_reset(void)
{
    monotonic_tick_count = 0U;
    tick_counter_overflowed = false;
    invalid_interrupt_observed = false;
    rollback_pending = false;
    initialized = false;
    timer_configuration_reset();
}

static enum kernel_time_status kernel_time_finish_failed_validation(
    enum apic_status stop_status
)
{
    if (stop_status != APIC_STATUS_OK) {
        rollback_pending = true;
        return KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    }

    kernel_time_state_reset();
    return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
}

static bool kernel_time_rollback_self_test(void)
{
    bool passed;

    monotonic_tick_count = UINT64_C(7);
    tick_counter_overflowed = true;
    invalid_interrupt_observed = true;
    timer_configuration.periodic_frequency_hz = UINT32_C(123);
    initialized = true;

    passed = kernel_time_finish_failed_validation(
            APIC_STATUS_INTERRUPT_FAILURE
        ) == KERNEL_TIME_STATUS_ROLLBACK_FAILURE &&
        initialized && rollback_pending &&
        monotonic_tick_count == UINT64_C(7) &&
        tick_counter_overflowed && invalid_interrupt_observed &&
        timer_configuration.periodic_frequency_hz == UINT32_C(123);
    passed = passed &&
        kernel_time_finish_failed_validation(APIC_STATUS_OK) ==
            KERNEL_TIME_STATUS_VALIDATION_FAILURE &&
        !initialized && !rollback_pending && monotonic_tick_count == 0U &&
        !tick_counter_overflowed && !invalid_interrupt_observed &&
        timer_configuration.periodic_frequency_hz == 0U;

    kernel_time_state_reset();
    return passed;
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
    struct spinlock_irq_token token;
    uint64_t ticks;
    bool overflowed;
    bool notify_scheduler = false;

    (void)context;

    if (!time_lock_acquire(&token)) {
        return;
    }

    if (rollback_pending) {
        invalid_interrupt_observed = true;
    } else if (frame == NULL || frame->vector != APIC_LOCAL_TIMER_VECTOR) {
        invalid_interrupt_observed = true;
    } else {
        ticks = monotonic_tick_count;
        overflowed = tick_counter_overflowed;
        tick_increment(&ticks, &overflowed);
        monotonic_tick_count = ticks;
        tick_counter_overflowed = overflowed;
        notify_scheduler = true;
    }

    if (!time_lock_exit(&token)) {
        return;
    }

    if (notify_scheduler) {
        scheduler_timer_tick();
    }
}

bool kernel_time_self_test(void)
{
    uint64_t nanoseconds;
    uint64_t ticks = UINT64_MAX - 1U;
    bool overflowed = false;
    const uint64_t period = UINT64_C(1000000);
    const uint64_t largest_convertible = UINT64_MAX / period;

    if (initialized || rollback_pending ||
        !kernel_time_rollback_self_test() ||
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

static enum kernel_time_status kernel_time_initialize_unguarded(
    uint32_t frequency_hz,
    bool caller_interrupts_enabled
)
{
    struct apic_local_timer_configuration candidate;
    enum apic_status apic_status;

    if (initialized && !rollback_pending) {
        return KERNEL_TIME_STATUS_ALREADY_INITIALIZED;
    }

    if (caller_interrupts_enabled) {
        return KERNEL_TIME_STATUS_INTERRUPTS_ENABLED;
    }

    if (rollback_pending) {
        apic_status = apic_local_timer_stop();
        if (apic_status != APIC_STATUS_OK) {
            return KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
        }
    }

    kernel_time_state_reset();
    apic_status = apic_local_timer_calibrate_and_start(
        frequency_hz,
        local_timer_handler,
        NULL,
        &candidate
    );

    if (apic_status != APIC_STATUS_OK) {
        if (apic_local_timer_active()) {
            initialized = true;
            rollback_pending = true;
            return KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
        }

        return time_status_from_apic(apic_status);
    }

    timer_configuration_copy(&candidate);
    initialized = true;

    if (kernel_time_validate_unguarded(false) != KERNEL_TIME_STATUS_OK) {
        return kernel_time_finish_failed_validation(
            apic_local_timer_stop()
        );
    }

    return KERNEL_TIME_STATUS_OK;
}

static enum kernel_time_status kernel_time_validate_unguarded(
    bool caller_interrupts_enabled
)
{
    if (time_poisoned) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    if (rollback_pending) {
        return KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    }

    if (!initialized) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (caller_interrupts_enabled) {
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

enum kernel_time_status kernel_time_initialize(uint32_t frequency_hz)
{
    struct spinlock_irq_token token;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();

    if (!time_lock_register() || !time_lock_acquire(&token)) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    return time_lock_finish(
        &token,
        kernel_time_initialize_unguarded(
            frequency_hz,
            interrupts_were_enabled
        )
    );
}

enum kernel_time_status kernel_time_validate(void)
{
    struct spinlock_irq_token token;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();

    if (!time_lock_acquire(&token)) {
        return time_lock_failure_status();
    }

    return time_lock_finish(
        &token,
        kernel_time_validate_unguarded(interrupts_were_enabled)
    );
}

uint64_t kernel_time_monotonic_ticks(void)
{
    struct spinlock_irq_token token;
    uint64_t ticks;

    if (!time_lock_acquire(&token)) {
        return 0U;
    }
    ticks = initialized && !rollback_pending ? monotonic_tick_count : 0U;
    return time_lock_exit(&token) ? ticks : 0U;
}

uint32_t kernel_time_frequency_hz(void)
{
    struct spinlock_irq_token token;
    uint32_t frequency;

    if (!time_lock_acquire(&token)) {
        return 0U;
    }
    frequency = initialized && !rollback_pending
        ? timer_configuration.periodic_frequency_hz
        : 0U;
    return time_lock_exit(&token) ? frequency : 0U;
}

uint64_t kernel_time_period_nanoseconds(void)
{
    struct spinlock_irq_token token;
    uint64_t period;

    if (!time_lock_acquire(&token)) {
        return 0U;
    }
    period = initialized && !rollback_pending
        ? timer_configuration.period_nanoseconds
        : 0U;
    return time_lock_exit(&token) ? period : 0U;
}

enum kernel_time_status kernel_time_ticks_to_nanoseconds(
    uint64_t ticks,
    uint64_t *nanoseconds
)
{
    struct spinlock_irq_token token;
    enum kernel_time_status status;

    if (nanoseconds == NULL) {
        return KERNEL_TIME_STATUS_NULL_ARGUMENT;
    }

    *nanoseconds = 0U;
    if (!time_lock_acquire(&token)) {
        return time_lock_failure_status();
    }

    if (rollback_pending) {
        status = KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    } else if (!initialized) {
        status = KERNEL_TIME_STATUS_NOT_INITIALIZED;
    } else {
        status = ticks_to_nanoseconds_with_period(
            ticks,
            timer_configuration.period_nanoseconds,
            nanoseconds
        );
    }

    if (!time_lock_exit(&token)) {
        *nanoseconds = 0U;
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }
    if (status != KERNEL_TIME_STATUS_OK) {
        *nanoseconds = 0U;
    }
    return status;
}

enum kernel_time_status kernel_time_monotonic_nanoseconds(
    uint64_t *nanoseconds
)
{
    struct spinlock_irq_token token;
    enum kernel_time_status status;

    if (nanoseconds == NULL) {
        return KERNEL_TIME_STATUS_NULL_ARGUMENT;
    }

    *nanoseconds = 0U;
    if (!time_lock_acquire(&token)) {
        return time_lock_failure_status();
    }

    if (rollback_pending) {
        status = KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    } else if (!initialized) {
        status = KERNEL_TIME_STATUS_NOT_INITIALIZED;
    } else if (tick_counter_overflowed) {
        status = KERNEL_TIME_STATUS_OVERFLOW;
    } else {
        status = ticks_to_nanoseconds_with_period(
            monotonic_tick_count,
            timer_configuration.period_nanoseconds,
            nanoseconds
        );
    }

    if (!time_lock_exit(&token)) {
        *nanoseconds = 0U;
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }
    if (status != KERNEL_TIME_STATUS_OK) {
        *nanoseconds = 0U;
    }
    return status;
}

enum kernel_time_status kernel_time_wait_for_ticks(uint64_t tick_count)
{
    struct spinlock_irq_token token;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    uint64_t target;
    enum kernel_time_status status;
    enum preempt_status preempt_status = PREEMPT_STATUS_OK;
    bool switch_allowed = true;
    bool reached = false;

    if (!time_lock_registered) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (!interrupts_were_enabled) {
        preempt_status = preempt_runtime_switch_allowed(&switch_allowed);
        if (preempt_status != PREEMPT_STATUS_OK || !switch_allowed) {
            return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
        }
    }

    if (!time_lock_acquire(&token)) {
        return time_lock_failure_status();
    }

    if (rollback_pending) {
        status = KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    } else if (!initialized) {
        status = KERNEL_TIME_STATUS_NOT_INITIALIZED;
    } else if (interrupts_were_enabled) {
        status = KERNEL_TIME_STATUS_INTERRUPTS_ENABLED;
    } else if (tick_counter_overflowed ||
               tick_count > UINT64_MAX - monotonic_tick_count) {
        status = KERNEL_TIME_STATUS_OVERFLOW;
    } else {
        status = KERNEL_TIME_STATUS_OK;
        target = monotonic_tick_count + tick_count;
    }

    if (!time_lock_exit(&token)) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }
    if (status != KERNEL_TIME_STATUS_OK || tick_count == 0U) {
        return status;
    }

    cpu_interrupt_enable();

    for (uint64_t attempt = 0U;
         attempt < KERNEL_TIME_WAIT_ATTEMPT_LIMIT;
         ++attempt) {
        if (monotonic_tick_count >= target) {
            reached = true;
            break;
        }

        __asm__ volatile ("pause" : : : "memory");
    }

    cpu_interrupt_disable();
    if (!time_lock_acquire(&token)) {
        return KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    if (rollback_pending) {
        status = KERNEL_TIME_STATUS_ROLLBACK_FAILURE;
    } else if (!initialized || invalid_interrupt_observed) {
        status = KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    } else if (tick_counter_overflowed) {
        status = KERNEL_TIME_STATUS_OVERFLOW;
    } else {
        status = reached
            ? KERNEL_TIME_STATUS_OK
            : KERNEL_TIME_STATUS_TIMEOUT;
    }

    return time_lock_finish(&token, status);
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
    case KERNEL_TIME_STATUS_ROLLBACK_FAILURE:
        return "Local APIC timer rollback failed; retry time initialization";
    default:
        return "unknown monotonic-time status";
    }
}

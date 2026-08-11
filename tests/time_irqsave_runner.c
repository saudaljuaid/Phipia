/* SPDX-License-Identifier: GPL-3.0-only */
#include <zenith/apic.h>
#include <zenith/cpu.h>
#include <zenith/scheduler.h>
#include <zenith/spinlock.h>
#include <zenith/time.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct spinlock *registered_lock;
static interrupt_handler_t timer_handler;
static void *timer_context;
static bool interrupts_enabled;
static bool timer_active;
static bool fail_next_release;
static bool scheduler_called_while_locked;
static bool preempt_switch_allowed = true;
static uint64_t next_generation = UINT64_C(1);
static size_t scheduler_tick_count;
static size_t interrupt_enable_count;
static size_t preempt_query_count;
static size_t time_lock_acquire_count;

void cpu_interrupt_disable(void)
{
    interrupts_enabled = false;
}

void cpu_interrupt_enable(void)
{
    interrupts_enabled = true;
    ++interrupt_enable_count;
}

bool cpu_interrupts_enabled(void)
{
    return interrupts_enabled;
}

enum preempt_status preempt_runtime_switch_allowed(bool *allowed)
{
    if (allowed == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    ++preempt_query_count;
    *allowed = preempt_switch_allowed;
    return PREEMPT_STATUS_OK;
}

enum spinlock_status spinlock_runtime_register(
    struct spinlock *lock,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
)
{
    if (lock == NULL || registered_lock != NULL || lock_index != 0U ||
        order_class != SPINLOCK_CLASS_TIME || order_rank != 0U) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }

    lock->word = 0U;
    lock->lock_index = lock_index;
    lock->registered = 1U;
    registered_lock = lock;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_irqsave_acquire(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    if (lock == NULL || token == NULL || lock != registered_lock ||
        lock->registered == 0U) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }
    if (lock->word != 0U) {
        return SPINLOCK_STATUS_LOCK_BUSY;
    }

    token->cpu_index = 0U;
    token->lock_index = lock->lock_index;
    token->generation = next_generation++;
    token->interrupts_were_enabled = interrupts_enabled;
    interrupts_enabled = false;
    lock->word = 1U;
    ++time_lock_acquire_count;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_irqrestore_release(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    if (lock == NULL || token == NULL || lock != registered_lock ||
        lock->word == 0U || token->lock_index != lock->lock_index) {
        interrupts_enabled = false;
        return SPINLOCK_STATUS_CORRUPTED;
    }
    if (fail_next_release) {
        fail_next_release = false;
        interrupts_enabled = false;
        return SPINLOCK_STATUS_CORRUPTED;
    }

    lock->word = 0U;
    interrupts_enabled = token->interrupts_were_enabled;
    token->lock_index = UINT32_MAX;
    return SPINLOCK_STATUS_OK;
}

enum apic_status apic_local_timer_calibrate_and_start(
    uint32_t periodic_frequency_hz,
    interrupt_handler_t handler,
    void *context,
    struct apic_local_timer_configuration *configuration
)
{
    if (periodic_frequency_hz == 0U || handler == NULL ||
        configuration == NULL || timer_active) {
        return APIC_STATUS_BAD_TIMER_FREQUENCY;
    }

    configuration->divide_configuration = UINT32_C(3);
    configuration->divisor = UINT32_C(16);
    configuration->calibrated_frequency_hz = UINT32_C(16000000);
    configuration->periodic_initial_count =
        configuration->calibrated_frequency_hz / periodic_frequency_hz;
    configuration->requested_frequency_hz = periodic_frequency_hz;
    configuration->periodic_frequency_hz = periodic_frequency_hz;
    configuration->minimum_sample_hz = UINT32_C(15900000);
    configuration->maximum_sample_hz = UINT32_C(16100000);
    configuration->period_nanoseconds =
        UINT64_C(1000000000) / periodic_frequency_hz;
    timer_handler = handler;
    timer_context = context;
    timer_active = true;
    return APIC_STATUS_OK;
}

enum apic_status apic_local_timer_stop(void)
{
    timer_active = false;
    return APIC_STATUS_OK;
}

enum apic_status apic_local_timer_validate(void)
{
    return timer_active ? APIC_STATUS_OK : APIC_STATUS_LOCAL_TIMER_NOT_ACTIVE;
}

bool apic_local_timer_active(void)
{
    return timer_active;
}

enum apic_status apic_validate(void)
{
    return APIC_STATUS_OK;
}

void scheduler_timer_tick(void)
{
    if (registered_lock == NULL || registered_lock->word != 0U) {
        scheduler_called_while_locked = true;
    }
    ++scheduler_tick_count;
}

static bool initialize_time(void)
{
    interrupts_enabled = false;
    return kernel_time_initialize(UINT32_C(1000)) == KERNEL_TIME_STATUS_OK &&
        timer_handler != NULL && timer_active && registered_lock != NULL &&
        registered_lock->word == 0U && !interrupts_enabled;
}

static void deliver_timer(uint8_t vector)
{
    struct interrupt_frame frame = {0};

    frame.vector = vector;
    interrupts_enabled = false;
    timer_handler(&frame, timer_context);
}

static bool run_scenario(const char *scenario)
{
    uint64_t nanoseconds = UINT64_MAX;

    if (strcmp(scenario, "pre-init") == 0) {
        interrupts_enabled = true;
        return kernel_time_validate() == KERNEL_TIME_STATUS_NOT_INITIALIZED &&
            interrupts_enabled && kernel_time_monotonic_ticks() == 0U &&
            kernel_time_ticks_to_nanoseconds(UINT64_C(1), &nanoseconds) ==
                KERNEL_TIME_STATUS_NOT_INITIALIZED &&
            nanoseconds == 0U && interrupts_enabled &&
            kernel_time_wait_for_ticks(UINT64_C(1)) ==
                KERNEL_TIME_STATUS_NOT_INITIALIZED &&
            interrupts_enabled && preempt_query_count == 0U;
    }

    if (strcmp(scenario, "init-if") == 0) {
        interrupts_enabled = true;
        if (kernel_time_initialize(UINT32_C(1000)) !=
                KERNEL_TIME_STATUS_INTERRUPTS_ENABLED ||
            !interrupts_enabled || timer_active) {
            return false;
        }
        interrupts_enabled = false;
        return kernel_time_initialize(UINT32_C(1000)) ==
                KERNEL_TIME_STATUS_OK &&
            !interrupts_enabled && timer_active && registered_lock != NULL &&
            registered_lock->word == 0U;
    }

    if (!initialize_time()) {
        return false;
    }

    if (strcmp(scenario, "deferred-scheduler") == 0) {
        deliver_timer(APIC_LOCAL_TIMER_VECTOR);
        return scheduler_tick_count == 1U &&
            !scheduler_called_while_locked && registered_lock->word == 0U &&
            kernel_time_monotonic_ticks() == UINT64_C(1);
    }

    if (strcmp(scenario, "exact-if") == 0) {
        interrupts_enabled = true;
        if (kernel_time_frequency_hz() != UINT32_C(1000) ||
            !interrupts_enabled ||
            kernel_time_period_nanoseconds() != UINT64_C(1000000) ||
            !interrupts_enabled ||
            kernel_time_ticks_to_nanoseconds(UINT64_C(3), &nanoseconds) !=
                KERNEL_TIME_STATUS_OK ||
            nanoseconds != UINT64_C(3000000) || !interrupts_enabled ||
            kernel_time_validate() != KERNEL_TIME_STATUS_INTERRUPTS_ENABLED ||
            !interrupts_enabled) {
            return false;
        }

        interrupts_enabled = false;
        return kernel_time_validate() == KERNEL_TIME_STATUS_OK &&
            !interrupts_enabled;
    }

    if (strcmp(scenario, "bad-frame") == 0) {
        deliver_timer(UINT8_C(0x31));
        return scheduler_tick_count == 0U &&
            !scheduler_called_while_locked && registered_lock->word == 0U &&
            kernel_time_validate() == KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    if (strcmp(scenario, "poison") == 0) {
        fail_next_release = true;
        return kernel_time_monotonic_nanoseconds(&nanoseconds) ==
                KERNEL_TIME_STATUS_VALIDATION_FAILURE &&
            nanoseconds == 0U && !interrupts_enabled &&
            registered_lock->word != 0U &&
            kernel_time_monotonic_ticks() == 0U &&
            kernel_time_validate() == KERNEL_TIME_STATUS_VALIDATION_FAILURE;
    }

    if (strcmp(scenario, "pinned-wait") == 0) {
        preempt_switch_allowed = false;
        return kernel_time_wait_for_ticks(UINT64_C(1)) ==
                KERNEL_TIME_STATUS_VALIDATION_FAILURE &&
            !interrupts_enabled && interrupt_enable_count == 0U &&
            preempt_query_count == 1U && time_lock_acquire_count == 1U &&
            registered_lock->word == 0U;
    }

    return false;
}

int main(int argument_count, char **arguments)
{
    if (argument_count != 2 || !run_scenario(arguments[1])) {
        fputs("time irqsave scenario failed\n", stderr);
        return 1;
    }

    puts("time irqsave scenario passed");
    return 0;
}

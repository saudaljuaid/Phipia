/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdio.h>

#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/percpu.h>
#include <zenith/preempt.h>

static bool test_interrupts_enabled = true;
static bool reschedule_observed_if_clear;
static unsigned int reschedule_count;

void cpu_interrupt_disable(void)
{
    test_interrupts_enabled = false;
}

void cpu_interrupt_enable(void)
{
    test_interrupts_enabled = true;
}

bool cpu_interrupts_enabled(void)
{
    return test_interrupts_enabled;
}

void cpu_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    struct cpu_cpuid_result *result
)
{
    (void)subleaf;
    result->eax = 0U;
    result->ebx = leaf == UINT32_C(1) ? UINT32_C(3) << 24U : 0U;
    result->ecx = 0U;
    result->edx = 0U;
}

void interrupt_trigger_reschedule(void)
{
    reschedule_observed_if_clear = !test_interrupts_enabled;
    ++reschedule_count;
    (void)preempt_runtime_take_reschedule();
}

static bool guard_is_clear(const struct preempt_guard *guard)
{
    return !guard->restore_interrupts && !guard->active &&
        !guard->accounted && !guard->runtime_present &&
        guard->cpu.cpu_index == PERCPU_INVALID_CPU &&
        guard->cpu.generation == 0U;
}

int main(void)
{
    const struct preempt_guard dirty = {
        .restore_interrupts = true,
        .active = true,
        .accounted = true,
        .runtime_present = true
    };
    struct preempt_guard inactive = {
        .restore_interrupts = true,
        .active = false,
        .accounted = true
    };
    struct preempt_guard outer = dirty;
    struct preempt_guard inner = dirty;
    struct preempt_guard normal = dirty;
    struct preempt_guard early = dirty;
    struct preempt_guard rejected = dirty;

    if (preempt_guard_enter(&early) != PREEMPT_STATUS_OK ||
        !early.active || early.runtime_present || test_interrupts_enabled ||
        percpu_runtime_initialize(0U, 3U) != PERCPU_STATUS_OK ||
        preempt_runtime_initialize() != PREEMPT_STATUS_OK ||
        preempt_guard_exit(&early) != PREEMPT_STATUS_OK ||
        !guard_is_clear(&early) || !test_interrupts_enabled ||
        reschedule_count != 0U ||
        preempt_guard_enter(&normal) != PREEMPT_STATUS_OK ||
        test_interrupts_enabled ||
        preempt_runtime_request_reschedule() != PREEMPT_STATUS_OK ||
        preempt_guard_exit(&normal) != PREEMPT_STATUS_OK ||
        !guard_is_clear(&normal) || !test_interrupts_enabled ||
        reschedule_count != 1U || !reschedule_observed_if_clear) {
        return 10;
    }

    test_interrupts_enabled = false;
    if (preempt_guard_exit(&inactive) != PREEMPT_STATUS_COUNTER_UNDERFLOW ||
        !guard_is_clear(&inactive) || !test_interrupts_enabled ||
        reschedule_count != 1U) {
        return 20;
    }

    if (preempt_guard_enter(&outer) != PREEMPT_STATUS_OK ||
        preempt_guard_enter(&inner) != PREEMPT_STATUS_OK ||
        preempt_runtime_request_reschedule() != PREEMPT_STATUS_OK ||
        preempt_guard_exit(&inner) != PREEMPT_STATUS_OK ||
        !guard_is_clear(&inner) || test_interrupts_enabled ||
        reschedule_count != 1U ||
        preempt_guard_exit(&outer) != PREEMPT_STATUS_OK ||
        !guard_is_clear(&outer) || !test_interrupts_enabled ||
        reschedule_count != 2U || !reschedule_observed_if_clear) {
        return 30;
    }

    if (preempt_guard_enter(&outer) != PREEMPT_STATUS_OK ||
        preempt_guard_enter(&inner) != PREEMPT_STATUS_OK ||
        test_interrupts_enabled ||
        preempt_runtime_request_reschedule() != PREEMPT_STATUS_OK ||
        preempt_guard_exit(&outer) != PREEMPT_STATUS_CORRUPTED ||
        !guard_is_clear(&outer) || !test_interrupts_enabled ||
        reschedule_count != 2U ||
        preempt_guard_exit(&inner) != PREEMPT_STATUS_POISONED ||
        !guard_is_clear(&inner) || test_interrupts_enabled ||
        reschedule_count != 2U) {
        return 40;
    }

    test_interrupts_enabled = true;
    if (preempt_guard_enter(&rejected) != PREEMPT_STATUS_POISONED ||
        !guard_is_clear(&rejected) || !test_interrupts_enabled ||
        reschedule_count != 2U) {
        return 50;
    }

    puts("preempt runtime guard cleanup passed");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdint.h>

#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/percpu.h>
#include <zenith/preempt.h>

#include "preempt_core.h"

static struct preempt_core_state runtime_state;
static bool runtime_initialized;
static uint32_t runtime_guard_depth[PREEMPT_CPU_LIMIT];

static uint32_t atomic_load_u32(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static uint8_t atomic_load_u8(const uint8_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void runtime_poison(void)
{
    __atomic_store_n(&runtime_state.poisoned, UINT8_C(1), __ATOMIC_RELEASE);
}

void preempt_runtime_fail_stop(void)
{
    cpu_interrupt_disable();
    if (runtime_initialized) {
        runtime_poison();
    }
}

static void restore_interrupt_state(bool enabled)
{
    if (enabled) {
        cpu_interrupt_enable();
    } else {
        cpu_interrupt_disable();
    }
}

static void guard_clear(struct preempt_guard *guard)
{
    guard->cpu.cpu_index = PERCPU_INVALID_CPU;
    guard->cpu.generation = 0U;
    guard->restore_interrupts = false;
    guard->active = false;
    guard->accounted = false;
    guard->runtime_present = false;
}

static enum preempt_status current_handle(struct percpu_handle *handle)
{
    const enum percpu_status status =
        percpu_runtime_current_handle(handle);

    if (status == PERCPU_STATUS_OK) {
        return PREEMPT_STATUS_OK;
    }
    if (status == PERCPU_STATUS_CPU_OFFLINE) {
        return PREEMPT_STATUS_CPU_OFFLINE;
    }
    if (status == PERCPU_STATUS_NOT_INITIALIZED) {
        return PREEMPT_STATUS_NOT_INITIALIZED;
    }
    return PREEMPT_STATUS_CORRUPTED;
}

static bool runtime_ready(uint32_t cpu_index)
{
    return runtime_initialized &&
        runtime_state.initialized == UINT8_C(1) &&
        atomic_load_u8(&runtime_state.poisoned) == 0U &&
        runtime_state.online_count != 0U &&
        cpu_index < PREEMPT_CPU_LIMIT &&
        atomic_load_u8(&runtime_state.cpus[cpu_index].online) == UINT8_C(1);
}

static bool runtime_reschedule_eligible(uint32_t cpu_index)
{
    const struct preempt_core_cpu *cpu = &runtime_state.cpus[cpu_index];

    return atomic_load_u32(&cpu->irq_depth) == 0U &&
        atomic_load_u32(&cpu->preempt_count) == 0U &&
        atomic_load_u8(&cpu->need_resched) == UINT8_C(1);
}

static enum preempt_status resolve_current(uint32_t *cpu_index)
{
    struct percpu_handle handle;
    enum preempt_status status;

    if (cpu_index == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *cpu_index = PERCPU_INVALID_CPU;
    status = current_handle(&handle);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    *cpu_index = handle.cpu_index;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_runtime_initialize(void)
{
    struct percpu_handle current;
    enum preempt_status current_status;
    enum preempt_status status;

    if (runtime_initialized) {
        return PREEMPT_STATUS_ALREADY_INITIALIZED;
    }
    current_status = current_handle(&current);
    if (current_status != PREEMPT_STATUS_OK) {
        return current_status;
    }

    preempt_core_clear(&runtime_state);
    status = preempt_core_initialize(&runtime_state, current.cpu_index);
    if (status == PREEMPT_STATUS_OK) {
        for (uint32_t index = 0U; index < PREEMPT_CPU_LIMIT; ++index) {
            runtime_guard_depth[index] = 0U;
        }
        runtime_initialized = true;
    }
    return status;
}

enum preempt_status preempt_runtime_cpu_online(uint32_t cpu_index)
{
    struct percpu_snapshot snapshot;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum preempt_status status;

    cpu_interrupt_disable();
    if (!runtime_initialized) {
        status = PREEMPT_STATUS_NOT_INITIALIZED;
    } else if (percpu_runtime_snapshot(cpu_index, &snapshot) !=
            PERCPU_STATUS_OK || !snapshot.online) {
        status = cpu_index >= PREEMPT_CPU_LIMIT ?
            PREEMPT_STATUS_INVALID_CPU : PREEMPT_STATUS_CPU_OFFLINE;
    } else {
        status = preempt_core_cpu_online(&runtime_state, cpu_index);
        if (status == PREEMPT_STATUS_OK) {
            runtime_guard_depth[cpu_index] = 0U;
        }
    }
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum preempt_status preempt_runtime_cpu_offline(uint32_t cpu_index)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum preempt_status status;

    cpu_interrupt_disable();
    if (!runtime_initialized) {
        status = PREEMPT_STATUS_NOT_INITIALIZED;
    } else if (cpu_index >= PREEMPT_CPU_LIMIT) {
        status = PREEMPT_STATUS_INVALID_CPU;
    } else if (runtime_guard_depth[cpu_index] != 0U) {
        status = PREEMPT_STATUS_CPU_BUSY;
    } else {
        status = preempt_core_cpu_offline(&runtime_state, cpu_index);
    }
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum preempt_status preempt_runtime_irq_enter(void)
{
    uint32_t cpu_index;
    struct preempt_core_cpu *cpu;
    enum preempt_status status = resolve_current(&cpu_index);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    if (!runtime_ready(cpu_index)) {
        return runtime_initialized ? PREEMPT_STATUS_POISONED :
            PREEMPT_STATUS_NOT_INITIALIZED;
    }
    cpu = &runtime_state.cpus[cpu_index];
    for (;;) {
        uint32_t observed = atomic_load_u32(&cpu->irq_depth);

        if (observed == PREEMPT_NESTING_LIMIT) {
            runtime_poison();
            return PREEMPT_STATUS_COUNTER_OVERFLOW;
        }
        if (__atomic_compare_exchange_n(
                &cpu->irq_depth, &observed, observed + 1U, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return PREEMPT_STATUS_OK;
        }
    }
}

enum preempt_status preempt_runtime_irq_exit(bool *reschedule_eligible)
{
    uint32_t cpu_index;
    struct preempt_core_cpu *cpu;
    enum preempt_status status;

    if (reschedule_eligible == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *reschedule_eligible = false;
    status = resolve_current(&cpu_index);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    if (!runtime_ready(cpu_index)) {
        return runtime_initialized ? PREEMPT_STATUS_POISONED :
            PREEMPT_STATUS_NOT_INITIALIZED;
    }
    cpu = &runtime_state.cpus[cpu_index];
    for (;;) {
        uint32_t observed = atomic_load_u32(&cpu->irq_depth);

        if (observed == 0U) {
            runtime_poison();
            return PREEMPT_STATUS_COUNTER_UNDERFLOW;
        }
        if (__atomic_compare_exchange_n(
                &cpu->irq_depth, &observed, observed - 1U, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    *reschedule_eligible = runtime_reschedule_eligible(cpu_index);
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_runtime_request_reschedule(void)
{
    uint32_t cpu_index;
    enum preempt_status status = resolve_current(&cpu_index);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    if (!runtime_ready(cpu_index)) {
        return runtime_initialized ? PREEMPT_STATUS_POISONED :
            PREEMPT_STATUS_NOT_INITIALIZED;
    }
    __atomic_store_n(
        &runtime_state.cpus[cpu_index].need_resched,
        UINT8_C(1), __ATOMIC_RELEASE
    );
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_runtime_take_reschedule(void)
{
    uint32_t cpu_index;
    enum preempt_status status = resolve_current(&cpu_index);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    if (!runtime_ready(cpu_index)) {
        return runtime_initialized ? PREEMPT_STATUS_POISONED :
            PREEMPT_STATUS_NOT_INITIALIZED;
    }
    if (!runtime_reschedule_eligible(cpu_index)) {
        return PREEMPT_STATUS_NOT_ELIGIBLE;
    }
    __atomic_store_n(
        &runtime_state.cpus[cpu_index].need_resched, 0U, __ATOMIC_RELEASE
    );
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_runtime_switch_allowed(bool *allowed)
{
    uint32_t cpu_index;
    enum preempt_status status;

    if (allowed == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *allowed = false;
    status = resolve_current(&cpu_index);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }
    if (!runtime_ready(cpu_index)) {
        return runtime_initialized ? PREEMPT_STATUS_POISONED :
            PREEMPT_STATUS_NOT_INITIALIZED;
    }
    *allowed = atomic_load_u32(
        &runtime_state.cpus[cpu_index].irq_depth
    ) == 0U && atomic_load_u32(
        &runtime_state.cpus[cpu_index].preempt_count
    ) == 0U;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_runtime_cpu_snapshot(
    uint32_t cpu_index,
    struct preempt_cpu_snapshot *snapshot
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum preempt_status status;

    if (snapshot == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    snapshot->irq_depth = 0U;
    snapshot->preempt_count = 0U;
    snapshot->need_resched = false;
    snapshot->online = false;
    cpu_interrupt_disable();
    status = runtime_initialized ?
        preempt_core_cpu_snapshot(&runtime_state, cpu_index, snapshot) :
        PREEMPT_STATUS_NOT_INITIALIZED;
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum preempt_status preempt_runtime_snapshot(struct preempt_cpu_snapshot *snapshot)
{
    uint32_t cpu_index;
    enum preempt_status status;

    if (snapshot == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    snapshot->irq_depth = 0U;
    snapshot->preempt_count = 0U;
    snapshot->need_resched = false;
    snapshot->online = false;
    status = resolve_current(&cpu_index);
    return status == PREEMPT_STATUS_OK ?
        preempt_runtime_cpu_snapshot(cpu_index, snapshot) : status;
}

enum preempt_status preempt_guard_enter(struct preempt_guard *guard)
{
    uint32_t cpu_index;
    enum preempt_status status;

    if (guard == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    guard_clear(guard);
    guard->restore_interrupts = cpu_interrupts_enabled();
    cpu_interrupt_disable();

    /* Early boot is already non-preemptible; support pre-runtime callers. */
    if (!runtime_initialized) {
        guard->active = true;
        return PREEMPT_STATUS_OK;
    }

    status = current_handle(&guard->cpu);
    if (status != PREEMPT_STATUS_OK) {
        restore_interrupt_state(guard->restore_interrupts);
        guard_clear(guard);
        return status;
    }
    cpu_index = guard->cpu.cpu_index;
    guard->runtime_present = true;
    if (!runtime_ready(cpu_index)) {
        restore_interrupt_state(guard->restore_interrupts);
        guard_clear(guard);
        return PREEMPT_STATUS_POISONED;
    }
    if (runtime_guard_depth[cpu_index] == PREEMPT_NESTING_LIMIT) {
        runtime_poison();
        restore_interrupt_state(guard->restore_interrupts);
        guard_clear(guard);
        return PREEMPT_STATUS_COUNTER_OVERFLOW;
    }
    if (runtime_guard_depth[cpu_index] != 0U) {
        ++runtime_guard_depth[cpu_index];
        guard->active = true;
        return PREEMPT_STATUS_OK;
    }
    if (atomic_load_u32(&runtime_state.cpus[cpu_index].preempt_count) ==
            PREEMPT_NESTING_LIMIT) {
        runtime_poison();
        restore_interrupt_state(guard->restore_interrupts);
        guard_clear(guard);
        return PREEMPT_STATUS_COUNTER_OVERFLOW;
    }

    (void)__atomic_add_fetch(
        &runtime_state.cpus[cpu_index].preempt_count, 1U,
        __ATOMIC_ACQ_REL
    );
    runtime_guard_depth[cpu_index] = 1U;
    guard->active = true;
    guard->accounted = true;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_guard_exit(struct preempt_guard *guard)
{
    struct percpu_handle current = {
        .cpu_index = PERCPU_INVALID_CPU,
        .generation = 0U
    };
    bool reschedule_eligible = false;
    bool restore_interrupts;
    uint32_t cpu_index = PERCPU_INVALID_CPU;
    enum preempt_status status;

    if (guard == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    restore_interrupts = guard->restore_interrupts;
    if (!guard->active) {
        status = PREEMPT_STATUS_COUNTER_UNDERFLOW;
    } else if (!guard->runtime_present) {
        status = PREEMPT_STATUS_OK;
    } else if (!runtime_initialized) {
        status = PREEMPT_STATUS_NOT_INITIALIZED;
    } else if (current_handle(&current) != PREEMPT_STATUS_OK ||
        current.cpu_index != guard->cpu.cpu_index ||
        current.generation != guard->cpu.generation) {
        runtime_poison();
        status = PREEMPT_STATUS_CORRUPTED;
    } else {
        cpu_index = current.cpu_index;
        if (!runtime_ready(cpu_index)) {
            status = PREEMPT_STATUS_POISONED;
        } else if (runtime_guard_depth[cpu_index] == 0U) {
            runtime_poison();
            status = PREEMPT_STATUS_COUNTER_UNDERFLOW;
        } else if (!guard->accounted) {
            --runtime_guard_depth[cpu_index];
            status = PREEMPT_STATUS_OK;
        } else if (runtime_guard_depth[cpu_index] != 1U) {
            runtime_poison();
            status = PREEMPT_STATUS_CORRUPTED;
        } else {
            runtime_guard_depth[cpu_index] = 0U;
            if (atomic_load_u32(
                    &runtime_state.cpus[cpu_index].preempt_count
                ) == 0U) {
                runtime_poison();
                status = PREEMPT_STATUS_COUNTER_UNDERFLOW;
            } else {
                (void)__atomic_sub_fetch(
                    &runtime_state.cpus[cpu_index].preempt_count, 1U,
                    __ATOMIC_ACQ_REL
                );
                reschedule_eligible =
                    runtime_reschedule_eligible(cpu_index);
                status = PREEMPT_STATUS_OK;
            }
        }
    }
    guard_clear(guard);

    /* Enter the DPL0 reschedule gate while IF remains clear. */
    if (status == PREEMPT_STATUS_OK && reschedule_eligible &&
        restore_interrupts) {
        interrupt_trigger_reschedule();
    }
    restore_interrupt_state(restore_interrupts);
    return status;
}

const char *preempt_status_string(enum preempt_status status)
{
    switch (status) {
    case PREEMPT_STATUS_OK:
        return "ok";
    case PREEMPT_STATUS_NULL_ARGUMENT:
        return "null preemption argument";
    case PREEMPT_STATUS_ALREADY_INITIALIZED:
        return "preemption state initialized twice";
    case PREEMPT_STATUS_NOT_INITIALIZED:
        return "preemption state is not initialized";
    case PREEMPT_STATUS_POISONED:
        return "preemption state is poisoned";
    case PREEMPT_STATUS_INVALID_CPU:
        return "invalid preemption CPU";
    case PREEMPT_STATUS_CPU_ALREADY_ONLINE:
        return "preemption CPU already online";
    case PREEMPT_STATUS_CPU_OFFLINE:
        return "preemption CPU offline";
    case PREEMPT_STATUS_LAST_CPU:
        return "cannot offline last preemption CPU";
    case PREEMPT_STATUS_CPU_BUSY:
        return "preemption CPU is busy";
    case PREEMPT_STATUS_COUNTER_OVERFLOW:
        return "preemption counter overflow";
    case PREEMPT_STATUS_COUNTER_UNDERFLOW:
        return "preemption counter underflow";
    case PREEMPT_STATUS_NOT_ELIGIBLE:
        return "reschedule is not eligible";
    case PREEMPT_STATUS_CORRUPTED:
        return "preemption state is corrupted";
    default:
        return "unknown preemption status";
    }
}

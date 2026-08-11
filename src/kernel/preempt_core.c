/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "preempt_core.h"

_Static_assert(sizeof(struct preempt_core_cpu) == 12U,
    "preemption CPU state must remain compact");
_Static_assert(sizeof(struct preempt_core_state) == 200U,
    "preemption core state size changed");

static bool cpu_is_eligible(const struct preempt_core_cpu *cpu)
{
    return cpu->online == UINT8_C(1) &&
        cpu->irq_depth == 0U &&
        cpu->preempt_count == 0U &&
        cpu->need_resched == UINT8_C(1);
}

void preempt_core_clear(struct preempt_core_state *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t index = 0U; index < PREEMPT_CPU_LIMIT; ++index) {
        state->cpus[index].irq_depth = 0U;
        state->cpus[index].preempt_count = 0U;
        state->cpus[index].need_resched = 0U;
        state->cpus[index].online = 0U;
        state->cpus[index].reserved[0] = 0U;
        state->cpus[index].reserved[1] = 0U;
    }

    state->online_count = 0U;
    state->initialized = 0U;
    state->poisoned = 0U;
    state->reserved[0] = 0U;
    state->reserved[1] = 0U;
}

enum preempt_status preempt_core_validate(
    const struct preempt_core_state *state
)
{
    uint32_t observed_online = 0U;

    if (state == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    if (state->poisoned > UINT8_C(1)) {
        return PREEMPT_STATUS_CORRUPTED;
    }

    if (state->poisoned == UINT8_C(1)) {
        return PREEMPT_STATUS_POISONED;
    }

    if (state->initialized == 0U) {
        return PREEMPT_STATUS_NOT_INITIALIZED;
    }

    if (state->initialized != UINT8_C(1)) {
        return PREEMPT_STATUS_CORRUPTED;
    }

    if (state->reserved[0] != 0U || state->reserved[1] != 0U) {
        return PREEMPT_STATUS_CORRUPTED;
    }

    for (size_t index = 0U; index < PREEMPT_CPU_LIMIT; ++index) {
        const struct preempt_core_cpu *cpu = &state->cpus[index];

        if (cpu->online > UINT8_C(1) || cpu->need_resched > UINT8_C(1) ||
            cpu->reserved[0] != 0U || cpu->reserved[1] != 0U ||
            cpu->irq_depth > PREEMPT_NESTING_LIMIT ||
            cpu->preempt_count > PREEMPT_NESTING_LIMIT) {
            return PREEMPT_STATUS_CORRUPTED;
        }

        if (cpu->online == 0U) {
            if (cpu->irq_depth != 0U || cpu->preempt_count != 0U ||
                cpu->need_resched != 0U) {
                return PREEMPT_STATUS_CORRUPTED;
            }
        } else {
            ++observed_online;
        }
    }

    if (observed_online == 0U ||
        observed_online != state->online_count ||
        state->online_count > PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_CORRUPTED;
    }

    return PREEMPT_STATUS_OK;
}

static enum preempt_status mutation_gate(struct preempt_core_state *state)
{
    enum preempt_status status;

    if (state == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    status = preempt_core_validate(state);
    if (status == PREEMPT_STATUS_CORRUPTED) {
        state->poisoned = UINT8_C(1);
    }

    return status;
}

static enum preempt_status online_cpu(
    struct preempt_core_state *state,
    uint32_t cpu_index,
    struct preempt_core_cpu **cpu
)
{
    enum preempt_status status = mutation_gate(state);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu_index >= PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_INVALID_CPU;
    }

    if (state->cpus[cpu_index].online == 0U) {
        return PREEMPT_STATUS_CPU_OFFLINE;
    }

    *cpu = &state->cpus[cpu_index];
    return PREEMPT_STATUS_OK;
}

static enum preempt_status poison_counter_fault(
    struct preempt_core_state *state,
    enum preempt_status status
)
{
    state->poisoned = UINT8_C(1);
    return status;
}

enum preempt_status preempt_core_initialize(
    struct preempt_core_state *state,
    uint32_t boot_cpu
)
{
    if (state == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    if (state->initialized != 0U) {
        return PREEMPT_STATUS_ALREADY_INITIALIZED;
    }

    if (boot_cpu >= PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_INVALID_CPU;
    }

    preempt_core_clear(state);
    state->cpus[boot_cpu].online = UINT8_C(1);
    state->online_count = 1U;
    state->initialized = UINT8_C(1);
    return preempt_core_validate(state);
}

enum preempt_status preempt_core_cpu_online(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    enum preempt_status status = mutation_gate(state);
    struct preempt_core_cpu *cpu;

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu_index >= PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_INVALID_CPU;
    }

    cpu = &state->cpus[cpu_index];
    if (cpu->online != 0U) {
        return PREEMPT_STATUS_CPU_ALREADY_ONLINE;
    }

    if (state->online_count >= PREEMPT_CPU_LIMIT) {
        state->poisoned = UINT8_C(1);
        return PREEMPT_STATUS_CORRUPTED;
    }

    cpu->online = UINT8_C(1);
    ++state->online_count;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_cpu_offline(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (state->online_count == 1U) {
        return PREEMPT_STATUS_LAST_CPU;
    }

    if (cpu->irq_depth != 0U || cpu->preempt_count != 0U ||
        cpu->need_resched != 0U) {
        return PREEMPT_STATUS_CPU_BUSY;
    }

    cpu->online = 0U;
    --state->online_count;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_irq_enter(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu->irq_depth == PREEMPT_NESTING_LIMIT) {
        return poison_counter_fault(state, PREEMPT_STATUS_COUNTER_OVERFLOW);
    }

    ++cpu->irq_depth;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_irq_exit(
    struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *reschedule_eligible
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status;

    if (reschedule_eligible == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *reschedule_eligible = false;

    status = online_cpu(state, cpu_index, &cpu);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu->irq_depth == 0U) {
        return poison_counter_fault(state, PREEMPT_STATUS_COUNTER_UNDERFLOW);
    }

    --cpu->irq_depth;
    *reschedule_eligible = cpu_is_eligible(cpu);
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_disable(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu->preempt_count == PREEMPT_NESTING_LIMIT) {
        return poison_counter_fault(state, PREEMPT_STATUS_COUNTER_OVERFLOW);
    }

    ++cpu->preempt_count;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_enable(
    struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *reschedule_eligible
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status;

    if (reschedule_eligible == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *reschedule_eligible = false;

    status = online_cpu(state, cpu_index, &cpu);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu->preempt_count == 0U) {
        return poison_counter_fault(state, PREEMPT_STATUS_COUNTER_UNDERFLOW);
    }

    --cpu->preempt_count;
    *reschedule_eligible = cpu_is_eligible(cpu);
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_request_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    cpu->need_resched = UINT8_C(1);
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_cancel_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    cpu->need_resched = 0U;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_reschedule_eligible(
    const struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *eligible
)
{
    enum preempt_status status;

    if (eligible == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }
    *eligible = false;

    status = preempt_core_validate(state);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu_index >= PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_INVALID_CPU;
    }

    if (state->cpus[cpu_index].online == 0U) {
        return PREEMPT_STATUS_CPU_OFFLINE;
    }

    *eligible = cpu_is_eligible(&state->cpus[cpu_index]);
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_take_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
)
{
    struct preempt_core_cpu *cpu;
    enum preempt_status status = online_cpu(state, cpu_index, &cpu);

    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (!cpu_is_eligible(cpu)) {
        return PREEMPT_STATUS_NOT_ELIGIBLE;
    }

    cpu->need_resched = 0U;
    return PREEMPT_STATUS_OK;
}

enum preempt_status preempt_core_cpu_snapshot(
    const struct preempt_core_state *state,
    uint32_t cpu_index,
    struct preempt_cpu_snapshot *snapshot
)
{
    enum preempt_status status;

    if (snapshot == NULL) {
        return PREEMPT_STATUS_NULL_ARGUMENT;
    }

    snapshot->irq_depth = 0U;
    snapshot->preempt_count = 0U;
    snapshot->need_resched = false;
    snapshot->online = false;

    status = preempt_core_validate(state);
    if (status != PREEMPT_STATUS_OK) {
        return status;
    }

    if (cpu_index >= PREEMPT_CPU_LIMIT) {
        return PREEMPT_STATUS_INVALID_CPU;
    }

    snapshot->irq_depth = state->cpus[cpu_index].irq_depth;
    snapshot->preempt_count = state->cpus[cpu_index].preempt_count;
    snapshot->need_resched = state->cpus[cpu_index].need_resched != 0U;
    snapshot->online = state->cpus[cpu_index].online != 0U;
    return PREEMPT_STATUS_OK;
}

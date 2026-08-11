/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "percpu_core.h"

_Static_assert(sizeof(struct percpu_core_cpu) == 12U,
    "per-CPU slot size changed");
_Static_assert(sizeof(struct percpu_core_state) == 208U,
    "per-CPU registry size changed");

void percpu_core_clear(struct percpu_core_state *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        state->cpus[index].apic_id = PERCPU_INVALID_APIC_ID;
        state->cpus[index].generation = 0U;
        state->cpus[index].online = 0U;
        state->cpus[index].boot_cpu = 0U;
        state->cpus[index].reserved[0] = 0U;
        state->cpus[index].reserved[1] = 0U;
    }

    state->online_count = 0U;
    state->next_generation = 1U;
    state->boot_cpu_index = PERCPU_INVALID_CPU;
    state->initialized = 0U;
    state->poisoned = 0U;
    state->reserved[0] = 0U;
    state->reserved[1] = 0U;
}

enum percpu_status percpu_core_validate(const struct percpu_core_state *state)
{
    uint32_t observed_online = 0U;
    uint32_t observed_boot = 0U;

    if (state == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    if (state->poisoned > UINT8_C(1)) {
        return PERCPU_STATUS_CORRUPTED;
    }
    if (state->poisoned == UINT8_C(1)) {
        return PERCPU_STATUS_POISONED;
    }
    if (state->initialized == 0U) {
        return PERCPU_STATUS_NOT_INITIALIZED;
    }
    if (state->initialized != UINT8_C(1) ||
        state->boot_cpu_index >= PERCPU_CPU_LIMIT ||
        state->next_generation == 0U || state->reserved[0] != 0U ||
        state->reserved[1] != 0U) {
        return PERCPU_STATUS_CORRUPTED;
    }

    for (size_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        const struct percpu_core_cpu *cpu = &state->cpus[index];

        if (cpu->online > UINT8_C(1) || cpu->boot_cpu > UINT8_C(1) ||
            cpu->reserved[0] != 0U || cpu->reserved[1] != 0U ||
            cpu->generation >= state->next_generation) {
            return PERCPU_STATUS_CORRUPTED;
        }

        if (cpu->online == 0U) {
            if (cpu->apic_id != PERCPU_INVALID_APIC_ID ||
                cpu->boot_cpu != 0U) {
                return PERCPU_STATUS_CORRUPTED;
            }
            continue;
        }

        if (cpu->apic_id == PERCPU_INVALID_APIC_ID || cpu->generation == 0U) {
            return PERCPU_STATUS_CORRUPTED;
        }
        ++observed_online;
        if (cpu->boot_cpu == UINT8_C(1)) {
            ++observed_boot;
            if (index != state->boot_cpu_index) {
                return PERCPU_STATUS_CORRUPTED;
            }
        }

        for (size_t other = index + 1U; other < PERCPU_CPU_LIMIT; ++other) {
            if (state->cpus[other].online == UINT8_C(1) &&
                state->cpus[other].apic_id == cpu->apic_id) {
                return PERCPU_STATUS_CORRUPTED;
            }
        }
    }

    if (observed_online == 0U || observed_online != state->online_count ||
        observed_online > PERCPU_CPU_LIMIT || observed_boot != 1U ||
        state->cpus[state->boot_cpu_index].online != UINT8_C(1) ||
        state->cpus[state->boot_cpu_index].boot_cpu != UINT8_C(1)) {
        return PERCPU_STATUS_CORRUPTED;
    }

    return PERCPU_STATUS_OK;
}

static enum percpu_status mutation_gate(struct percpu_core_state *state)
{
    enum percpu_status status;

    if (state == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    status = percpu_core_validate(state);
    if (status == PERCPU_STATUS_CORRUPTED) {
        state->poisoned = UINT8_C(1);
    }
    return status;
}

static bool apic_id_is_online(
    const struct percpu_core_state *state,
    uint32_t apic_id
)
{
    for (size_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        if (state->cpus[index].online == UINT8_C(1) &&
            state->cpus[index].apic_id == apic_id) {
            return true;
        }
    }
    return false;
}

enum percpu_status percpu_core_initialize(
    struct percpu_core_state *state,
    uint32_t boot_cpu_index,
    uint32_t boot_apic_id
)
{
    if (state == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    if (state->initialized != 0U) {
        return PERCPU_STATUS_ALREADY_INITIALIZED;
    }
    if (boot_cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    if (boot_apic_id == PERCPU_INVALID_APIC_ID) {
        return PERCPU_STATUS_INVALID_APIC_ID;
    }

    percpu_core_clear(state);
    state->cpus[boot_cpu_index].apic_id = boot_apic_id;
    state->cpus[boot_cpu_index].generation = state->next_generation;
    state->cpus[boot_cpu_index].online = UINT8_C(1);
    state->cpus[boot_cpu_index].boot_cpu = UINT8_C(1);
    ++state->next_generation;
    state->online_count = 1U;
    state->boot_cpu_index = boot_cpu_index;
    state->initialized = UINT8_C(1);
    return percpu_core_validate(state);
}

enum percpu_status percpu_core_cpu_online(
    struct percpu_core_state *state,
    uint32_t cpu_index,
    uint32_t apic_id
)
{
    enum percpu_status status = mutation_gate(state);

    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    if (apic_id == PERCPU_INVALID_APIC_ID) {
        return PERCPU_STATUS_INVALID_APIC_ID;
    }
    if (state->cpus[cpu_index].online == UINT8_C(1)) {
        return PERCPU_STATUS_CPU_ALREADY_ONLINE;
    }
    if (apic_id_is_online(state, apic_id)) {
        return PERCPU_STATUS_DUPLICATE_APIC_ID;
    }
    if (state->next_generation == UINT32_MAX) {
        state->poisoned = UINT8_C(1);
        return PERCPU_STATUS_GENERATION_EXHAUSTED;
    }

    state->cpus[cpu_index].apic_id = apic_id;
    state->cpus[cpu_index].generation = state->next_generation;
    state->cpus[cpu_index].online = UINT8_C(1);
    ++state->next_generation;
    ++state->online_count;
    return PERCPU_STATUS_OK;
}

enum percpu_status percpu_core_cpu_offline(
    struct percpu_core_state *state,
    uint32_t cpu_index
)
{
    enum percpu_status status = mutation_gate(state);

    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    if (state->cpus[cpu_index].online == 0U) {
        return PERCPU_STATUS_CPU_OFFLINE;
    }
    if (state->cpus[cpu_index].boot_cpu == UINT8_C(1)) {
        return PERCPU_STATUS_BOOT_CPU;
    }

    state->cpus[cpu_index].apic_id = PERCPU_INVALID_APIC_ID;
    state->cpus[cpu_index].online = 0U;
    --state->online_count;
    return PERCPU_STATUS_OK;
}

enum percpu_status percpu_core_lookup_apic(
    const struct percpu_core_state *state,
    uint32_t apic_id,
    uint32_t *cpu_index
)
{
    enum percpu_status status;

    if (cpu_index == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    *cpu_index = PERCPU_INVALID_CPU;
    status = percpu_core_validate(state);
    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (apic_id == PERCPU_INVALID_APIC_ID) {
        return PERCPU_STATUS_INVALID_APIC_ID;
    }

    for (size_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        if (state->cpus[index].online == UINT8_C(1) &&
            state->cpus[index].apic_id == apic_id) {
            *cpu_index = (uint32_t)index;
            return PERCPU_STATUS_OK;
        }
    }
    return PERCPU_STATUS_CPU_OFFLINE;
}

enum percpu_status percpu_core_handle(
    const struct percpu_core_state *state,
    uint32_t cpu_index,
    struct percpu_handle *handle
)
{
    enum percpu_status status;

    if (handle == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    handle->cpu_index = PERCPU_INVALID_CPU;
    handle->generation = 0U;
    status = percpu_core_validate(state);
    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    if (state->cpus[cpu_index].online == 0U) {
        return PERCPU_STATUS_CPU_OFFLINE;
    }
    handle->cpu_index = cpu_index;
    handle->generation = state->cpus[cpu_index].generation;
    return PERCPU_STATUS_OK;
}

enum percpu_status percpu_core_resolve(
    const struct percpu_core_state *state,
    const struct percpu_handle *handle,
    uint32_t *cpu_index
)
{
    enum percpu_status status;

    if (handle == NULL || cpu_index == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    *cpu_index = PERCPU_INVALID_CPU;
    status = percpu_core_validate(state);
    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (handle->cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    if (state->cpus[handle->cpu_index].online == 0U ||
        handle->generation == 0U ||
        state->cpus[handle->cpu_index].generation != handle->generation) {
        return PERCPU_STATUS_STALE_HANDLE;
    }
    *cpu_index = handle->cpu_index;
    return PERCPU_STATUS_OK;
}

enum percpu_status percpu_core_snapshot(
    const struct percpu_core_state *state,
    uint32_t cpu_index,
    struct percpu_snapshot *snapshot
)
{
    enum percpu_status status;

    if (snapshot == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    snapshot->apic_id = PERCPU_INVALID_APIC_ID;
    snapshot->generation = 0U;
    snapshot->online = false;
    snapshot->boot_cpu = false;
    status = percpu_core_validate(state);
    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return PERCPU_STATUS_INVALID_CPU;
    }
    snapshot->apic_id = state->cpus[cpu_index].apic_id;
    snapshot->generation = state->cpus[cpu_index].generation;
    snapshot->online = state->cpus[cpu_index].online == UINT8_C(1);
    snapshot->boot_cpu = state->cpus[cpu_index].boot_cpu == UINT8_C(1);
    return PERCPU_STATUS_OK;
}

bool percpu_core_is_online(
    const struct percpu_core_state *state,
    uint32_t cpu_index
)
{
    return percpu_core_validate(state) == PERCPU_STATUS_OK &&
        cpu_index < PERCPU_CPU_LIMIT &&
        state->cpus[cpu_index].online == UINT8_C(1);
}

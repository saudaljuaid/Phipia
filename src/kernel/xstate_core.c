/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xstate_core.h"

_Static_assert(sizeof(struct xstate_layout) == 32U,
    "XSAVE layout ABI changed");
_Static_assert(sizeof(struct xstate_core_task) == 32U,
    "XSAVE task metadata must remain compact");
_Static_assert(sizeof(struct xstate_core_cpu) == 16U,
    "XSAVE CPU metadata must remain compact");
_Static_assert(sizeof(struct xstate_core_state) == 824U,
    "XSAVE core state size changed");

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void clear_handle(struct xstate_task_handle *handle)
{
    handle->index = XSTATE_INVALID_INDEX;
    handle->generation = 0U;
}

static void clear_task(struct xstate_core_task *task, uint32_t generation)
{
    task->save_count = 0U;
    task->restore_count = 0U;
    task->generation = generation;
    task->owner_cpu = XSTATE_INVALID_INDEX;
    task->state = (uint8_t)XSTATE_TASK_FREE;
    task->image_ready = 0U;
    for (size_t index = 0U; index < sizeof(task->reserved); ++index) {
        task->reserved[index] = 0U;
    }
}

static void clear_cpu(struct xstate_core_cpu *cpu)
{
    cpu->loaded_task = XSTATE_INVALID_INDEX;
    cpu->loaded_generation = 0U;
    cpu->online = 0U;
    for (size_t index = 0U; index < sizeof(cpu->reserved); ++index) {
        cpu->reserved[index] = 0U;
    }
}

void xstate_core_clear(struct xstate_core_state *state)
{
    if (state == NULL) {
        return;
    }

    state->layout.enabled_mask = 0U;
    state->layout.bounded_hardware_mask = 0U;
    state->layout.area_size = 0U;
    state->layout.alignment = 0U;
    state->layout.avx_component_size = 0U;
    state->layout.avx_component_offset = 0U;
    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        clear_task(&state->tasks[index], 0U);
    }
    for (size_t index = 0U; index < XSTATE_CPU_LIMIT; ++index) {
        clear_cpu(&state->cpus[index]);
    }
    state->active_tasks = 0U;
    state->retired_tasks = 0U;
    state->online_cpus = 0U;
    state->unexpected_nm_count = 0U;
    state->initialized = 0U;
    state->poisoned = 0U;
    for (size_t index = 0U; index < sizeof(state->reserved); ++index) {
        state->reserved[index] = 0U;
    }
}

enum xstate_status xstate_layout_derive(
    const struct xstate_layout_probe *probe,
    struct xstate_layout *layout
)
{
    uint64_t avx_end;
    uint32_t rounded_size;

    if (probe == NULL || layout == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    layout->enabled_mask = 0U;
    layout->bounded_hardware_mask = 0U;
    layout->area_size = 0U;
    layout->alignment = 0U;
    layout->avx_component_size = 0U;
    layout->avx_component_offset = 0U;

    if (probe->xsave != UINT8_C(1)) {
        return XSTATE_STATUS_XSAVE_UNAVAILABLE;
    }
    if (probe->osxsave != UINT8_C(1)) {
        return XSTATE_STATUS_OSXSAVE_UNAVAILABLE;
    }
    if (probe->leaf_d != UINT8_C(1)) {
        return XSTATE_STATUS_CPUID_LEAF_UNAVAILABLE;
    }
    if (probe->reserved != 0U ||
        (probe->cpuid_xcr0_mask & XSTATE_X87_MASK) == 0U ||
        ((probe->cpuid_xcr0_mask & XSTATE_AVX_MASK) != 0U &&
            (probe->cpuid_xcr0_mask & XSTATE_SSE_MASK) == 0U) ||
        (probe->xcr0 & XSTATE_X87_MASK) == 0U ||
        (probe->xcr0 & ~probe->cpuid_xcr0_mask) != 0U) {
        return XSTATE_STATUS_INVALID_XCR0;
    }
    if ((probe->xcr0 & ~XSTATE_BOUNDED_MASK) != 0U) {
        return XSTATE_STATUS_UNSUPPORTED_FEATURE;
    }
    if ((probe->xcr0 & XSTATE_AVX_MASK) != 0U &&
        (probe->xcr0 & XSTATE_SSE_MASK) == 0U) {
        return XSTATE_STATUS_INVALID_XCR0;
    }
    if (probe->enabled_area_size < XSTATE_MINIMUM_AREA_SIZE ||
        probe->enabled_area_size > probe->maximum_area_size ||
        probe->enabled_area_size > XSTATE_MAXIMUM_AREA_SIZE) {
        return XSTATE_STATUS_INVALID_AREA_SIZE;
    }

    if ((probe->cpuid_xcr0_mask & XSTATE_AVX_MASK) != 0U) {
        avx_end = (uint64_t)probe->avx_component_offset +
            (uint64_t)probe->avx_component_size;
        if (probe->avx_component_size != UINT32_C(256) ||
            probe->avx_component_offset < XSTATE_MINIMUM_AREA_SIZE ||
            (probe->avx_component_offset % XSTATE_AREA_ALIGNMENT) != 0U ||
            avx_end > probe->maximum_area_size) {
            return XSTATE_STATUS_INVALID_COMPONENT;
        }
        if ((probe->xcr0 & XSTATE_AVX_MASK) != 0U &&
            avx_end > probe->enabled_area_size) {
            return XSTATE_STATUS_INVALID_COMPONENT;
        }
    } else if (probe->avx_component_size != 0U ||
        probe->avx_component_offset != 0U) {
        return XSTATE_STATUS_INVALID_COMPONENT;
    }

    rounded_size = (probe->enabled_area_size +
        (XSTATE_AREA_ALIGNMENT - 1U)) & ~(XSTATE_AREA_ALIGNMENT - 1U);
    if (rounded_size > XSTATE_MAXIMUM_AREA_SIZE) {
        return XSTATE_STATUS_INVALID_AREA_SIZE;
    }

    layout->enabled_mask = probe->xcr0;
    layout->bounded_hardware_mask =
        probe->cpuid_xcr0_mask & XSTATE_BOUNDED_MASK;
    layout->area_size = rounded_size;
    layout->alignment = XSTATE_AREA_ALIGNMENT;
    if ((probe->xcr0 & XSTATE_AVX_MASK) != 0U) {
        layout->avx_component_size = probe->avx_component_size;
        layout->avx_component_offset = probe->avx_component_offset;
    }
    return xstate_layout_validate(layout);
}

enum xstate_status xstate_layout_validate(const struct xstate_layout *layout)
{
    uint64_t avx_end;

    if (layout == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    if ((layout->bounded_hardware_mask & XSTATE_X87_MASK) == 0U ||
        (layout->bounded_hardware_mask & ~XSTATE_BOUNDED_MASK) != 0U ||
        ((layout->bounded_hardware_mask & XSTATE_AVX_MASK) != 0U &&
            (layout->bounded_hardware_mask & XSTATE_SSE_MASK) == 0U) ||
        (layout->enabled_mask & XSTATE_X87_MASK) == 0U ||
        (layout->enabled_mask & ~layout->bounded_hardware_mask) != 0U ||
        (layout->enabled_mask & ~XSTATE_BOUNDED_MASK) != 0U ||
        ((layout->enabled_mask & XSTATE_AVX_MASK) != 0U &&
            (layout->enabled_mask & XSTATE_SSE_MASK) == 0U)) {
        return XSTATE_STATUS_INVALID_XCR0;
    }
    if (layout->alignment != XSTATE_AREA_ALIGNMENT ||
        layout->area_size < XSTATE_MINIMUM_AREA_SIZE ||
        layout->area_size > XSTATE_MAXIMUM_AREA_SIZE ||
        (layout->area_size % XSTATE_AREA_ALIGNMENT) != 0U) {
        return XSTATE_STATUS_INVALID_AREA_SIZE;
    }

    if ((layout->enabled_mask & XSTATE_AVX_MASK) != 0U) {
        avx_end = (uint64_t)layout->avx_component_offset +
            (uint64_t)layout->avx_component_size;
        if (layout->avx_component_size != UINT32_C(256) ||
            layout->avx_component_offset < XSTATE_MINIMUM_AREA_SIZE ||
            (layout->avx_component_offset % XSTATE_AREA_ALIGNMENT) != 0U ||
            avx_end > layout->area_size) {
            return XSTATE_STATUS_INVALID_COMPONENT;
        }
    } else if (layout->avx_component_size != 0U ||
        layout->avx_component_offset != 0U) {
        return XSTATE_STATUS_INVALID_COMPONENT;
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_area_validate(
    const struct xstate_layout *layout,
    uintptr_t address,
    size_t byte_count
)
{
    enum xstate_status status = xstate_layout_validate(layout);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (address == (uintptr_t)0U ||
        (address % (uintptr_t)layout->alignment) != (uintptr_t)0U) {
        return XSTATE_STATUS_MISALIGNED_AREA;
    }
    if (byte_count < (size_t)layout->area_size) {
        return XSTATE_STATUS_AREA_TOO_SMALL;
    }
    if (address > UINTPTR_MAX - ((uintptr_t)layout->area_size - 1U)) {
        return XSTATE_STATUS_INVALID_AREA_SIZE;
    }
    return XSTATE_STATUS_OK;
}

static enum xstate_status validate_live_state(
    const struct xstate_core_state *state
)
{
    uint32_t observed_active = 0U;
    uint32_t observed_retired = 0U;
    uint32_t observed_online = 0U;

    for (size_t index = 0U; index < XSTATE_CPU_LIMIT; ++index) {
        const struct xstate_core_cpu *cpu = &state->cpus[index];

        if (cpu->online > UINT8_C(1) ||
            !bytes_are_zero(cpu->reserved, sizeof(cpu->reserved))) {
            return XSTATE_STATUS_CORRUPTED;
        }
        if (cpu->online == 0U) {
            if (cpu->loaded_task != XSTATE_INVALID_INDEX ||
                cpu->loaded_generation != 0U) {
                return XSTATE_STATUS_CORRUPTED;
            }
            continue;
        }
        ++observed_online;
        if (cpu->loaded_task == XSTATE_INVALID_INDEX) {
            if (cpu->loaded_generation != 0U) {
                return XSTATE_STATUS_CORRUPTED;
            }
        } else if (cpu->loaded_task >= XSTATE_TASK_LIMIT ||
            cpu->loaded_generation == 0U) {
            return XSTATE_STATUS_CORRUPTED;
        }
    }

    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        const struct xstate_core_task *task = &state->tasks[index];
        enum xstate_task_state task_state =
            (enum xstate_task_state)task->state;

        if (task_state > XSTATE_TASK_RETIRED || task->image_ready > UINT8_C(1) ||
            !bytes_are_zero(task->reserved, sizeof(task->reserved))) {
            return XSTATE_STATUS_CORRUPTED;
        }
        if (task_state == XSTATE_TASK_FREE) {
            if (task->generation == UINT32_MAX || task->image_ready != 0U ||
                task->owner_cpu != XSTATE_INVALID_INDEX ||
                task->save_count != 0U || task->restore_count != 0U) {
                return XSTATE_STATUS_CORRUPTED;
            }
            continue;
        }
        if (task_state == XSTATE_TASK_RETIRED) {
            ++observed_retired;
            if (task->generation != UINT32_MAX || task->image_ready != 0U ||
                task->owner_cpu != XSTATE_INVALID_INDEX ||
                task->save_count != 0U || task->restore_count != 0U) {
                return XSTATE_STATUS_CORRUPTED;
            }
            continue;
        }

        ++observed_active;
        if (task->generation == 0U) {
            return XSTATE_STATUS_CORRUPTED;
        }
        if (task_state == XSTATE_TASK_BUILDING) {
            if (task->owner_cpu != XSTATE_INVALID_INDEX ||
                task->save_count != 0U || task->restore_count != 0U) {
                return XSTATE_STATUS_CORRUPTED;
            }
            continue;
        }
        if (task->image_ready != UINT8_C(1)) {
            return XSTATE_STATUS_CORRUPTED;
        }
        if (task_state == XSTATE_TASK_DYING &&
            task->owner_cpu != XSTATE_INVALID_INDEX) {
            return XSTATE_STATUS_CORRUPTED;
        }
        if (task->owner_cpu == XSTATE_INVALID_INDEX) {
            if (task->restore_count != task->save_count) {
                return XSTATE_STATUS_CORRUPTED;
            }
        } else {
            const struct xstate_core_cpu *owner;

            if (task_state != XSTATE_TASK_LIVE ||
                task->owner_cpu >= XSTATE_CPU_LIMIT ||
                task->save_count == UINT64_MAX ||
                task->restore_count != task->save_count + UINT64_C(1)) {
                return XSTATE_STATUS_CORRUPTED;
            }
            owner = &state->cpus[task->owner_cpu];
            if (owner->online != UINT8_C(1) ||
                owner->loaded_task != index ||
                owner->loaded_generation != task->generation) {
                return XSTATE_STATUS_CORRUPTED;
            }
        }
    }

    for (size_t index = 0U; index < XSTATE_CPU_LIMIT; ++index) {
        const struct xstate_core_cpu *cpu = &state->cpus[index];

        if (cpu->online == UINT8_C(1) &&
            cpu->loaded_task != XSTATE_INVALID_INDEX) {
            const struct xstate_core_task *task =
                &state->tasks[cpu->loaded_task];

            if (task->state != (uint8_t)XSTATE_TASK_LIVE ||
                task->generation != cpu->loaded_generation ||
                task->owner_cpu != index) {
                return XSTATE_STATUS_CORRUPTED;
            }
        }
    }

    if (observed_active != state->active_tasks ||
        observed_retired != state->retired_tasks ||
        observed_online == 0U || observed_online != state->online_cpus) {
        return XSTATE_STATUS_CORRUPTED;
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_validate(const struct xstate_core_state *state)
{
    enum xstate_status status;

    if (state == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    if (state->poisoned > UINT8_C(1)) {
        return XSTATE_STATUS_CORRUPTED;
    }
    if (state->poisoned == UINT8_C(1)) {
        return XSTATE_STATUS_POISONED;
    }
    if (state->initialized == 0U) {
        return XSTATE_STATUS_NOT_INITIALIZED;
    }
    if (state->initialized != UINT8_C(1) ||
        !bytes_are_zero(state->reserved, sizeof(state->reserved)) ||
        state->unexpected_nm_count != 0U) {
        return XSTATE_STATUS_CORRUPTED;
    }
    status = xstate_layout_validate(&state->layout);
    if (status != XSTATE_STATUS_OK) {
        return XSTATE_STATUS_CORRUPTED;
    }
    return validate_live_state(state);
}

enum xstate_status xstate_core_initialize(
    struct xstate_core_state *state,
    const struct xstate_layout *layout,
    uint32_t boot_cpu
)
{
    enum xstate_status status;

    if (state == NULL || layout == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    if (state->initialized != 0U) {
        return XSTATE_STATUS_ALREADY_INITIALIZED;
    }
    if (boot_cpu >= XSTATE_CPU_LIMIT) {
        return XSTATE_STATUS_INVALID_CPU;
    }
    status = xstate_layout_validate(layout);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }

    xstate_core_clear(state);
    state->layout = *layout;
    state->cpus[boot_cpu].online = UINT8_C(1);
    state->online_cpus = 1U;
    state->initialized = UINT8_C(1);
    return xstate_core_validate(state);
}

static enum xstate_status mutation_gate(struct xstate_core_state *state)
{
    enum xstate_status status;

    if (state == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    status = xstate_core_validate(state);
    if (status == XSTATE_STATUS_CORRUPTED) {
        state->poisoned = UINT8_C(1);
    }
    return status;
}

static enum xstate_status online_cpu(
    struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_core_cpu **cpu
)
{
    enum xstate_status status = mutation_gate(state);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (cpu_index >= XSTATE_CPU_LIMIT) {
        return XSTATE_STATUS_INVALID_CPU;
    }
    if (state->cpus[cpu_index].online == 0U) {
        return XSTATE_STATUS_CPU_OFFLINE;
    }
    *cpu = &state->cpus[cpu_index];
    return XSTATE_STATUS_OK;
}

static enum xstate_status resolve_task(
    struct xstate_core_state *state,
    struct xstate_task_handle handle,
    struct xstate_core_task **task
)
{
    if (handle.index >= XSTATE_TASK_LIMIT || handle.generation == 0U) {
        return XSTATE_STATUS_INVALID_HANDLE;
    }
    if (state->tasks[handle.index].state == (uint8_t)XSTATE_TASK_FREE ||
        state->tasks[handle.index].state == (uint8_t)XSTATE_TASK_RETIRED ||
        state->tasks[handle.index].generation != handle.generation) {
        return XSTATE_STATUS_STALE_HANDLE;
    }
    *task = &state->tasks[handle.index];
    return XSTATE_STATUS_OK;
}

static enum xstate_status resolve_const_task(
    const struct xstate_core_state *state,
    struct xstate_task_handle handle,
    const struct xstate_core_task **task
)
{
    if (handle.index >= XSTATE_TASK_LIMIT || handle.generation == 0U) {
        return XSTATE_STATUS_INVALID_HANDLE;
    }
    if (state->tasks[handle.index].state == (uint8_t)XSTATE_TASK_FREE ||
        state->tasks[handle.index].state == (uint8_t)XSTATE_TASK_RETIRED ||
        state->tasks[handle.index].generation != handle.generation) {
        return XSTATE_STATUS_STALE_HANDLE;
    }
    *task = &state->tasks[handle.index];
    return XSTATE_STATUS_OK;
}

static enum xstate_status mutable_task(
    struct xstate_core_state *state,
    struct xstate_task_handle handle,
    struct xstate_core_task **task
)
{
    enum xstate_status status = mutation_gate(state);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    return resolve_task(state, handle, task);
}

enum xstate_status xstate_core_cpu_online(
    struct xstate_core_state *state,
    uint32_t cpu_index
)
{
    enum xstate_status status = mutation_gate(state);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (cpu_index >= XSTATE_CPU_LIMIT) {
        return XSTATE_STATUS_INVALID_CPU;
    }
    if (state->cpus[cpu_index].online == UINT8_C(1)) {
        return XSTATE_STATUS_CPU_ALREADY_ONLINE;
    }
    clear_cpu(&state->cpus[cpu_index]);
    state->cpus[cpu_index].online = UINT8_C(1);
    ++state->online_cpus;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_cpu_offline(
    struct xstate_core_state *state,
    uint32_t cpu_index
)
{
    struct xstate_core_cpu *cpu;
    enum xstate_status status = online_cpu(state, cpu_index, &cpu);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (state->online_cpus == 1U) {
        return XSTATE_STATUS_LAST_CPU;
    }
    if (cpu->loaded_task != XSTATE_INVALID_INDEX) {
        return XSTATE_STATUS_CPU_BUSY;
    }
    clear_cpu(cpu);
    --state->online_cpus;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_task_create(
    struct xstate_core_state *state,
    struct xstate_task_handle *handle
)
{
    enum xstate_status status;

    if (handle == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    clear_handle(handle);
    status = mutation_gate(state);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }

    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        struct xstate_core_task *task = &state->tasks[index];

        if (task->state == (uint8_t)XSTATE_TASK_FREE) {
            ++task->generation;
            task->state = (uint8_t)XSTATE_TASK_BUILDING;
            ++state->active_tasks;
            handle->index = (uint32_t)index;
            handle->generation = task->generation;
            return XSTATE_STATUS_OK;
        }
    }
    if (state->retired_tasks == XSTATE_TASK_LIMIT) {
        state->poisoned = UINT8_C(1);
        return XSTATE_STATUS_GENERATION_EXHAUSTED;
    }
    return XSTATE_STATUS_NO_TASK_SLOT;
}

enum xstate_status xstate_core_task_mark_image_ready(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_BUILDING) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (task->image_ready == UINT8_C(1)) {
        return XSTATE_STATUS_IMAGE_ALREADY_READY;
    }
    task->image_ready = UINT8_C(1);
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_task_publish(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_BUILDING) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (task->image_ready == 0U) {
        return XSTATE_STATUS_IMAGE_NOT_READY;
    }
    task->state = (uint8_t)XSTATE_TASK_LIVE;
    return XSTATE_STATUS_OK;
}

static void release_task_slot(
    struct xstate_core_state *state,
    struct xstate_core_task *task
)
{
    uint32_t generation = task->generation;

    --state->active_tasks;
    clear_task(task, generation);
    if (generation == UINT32_MAX) {
        task->state = (uint8_t)XSTATE_TASK_RETIRED;
        ++state->retired_tasks;
    }
}

enum xstate_status xstate_core_task_abort(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_BUILDING) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    release_task_slot(state, task);
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_record_save(
    struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_task_handle handle
)
{
    struct xstate_core_cpu *cpu;
    struct xstate_core_task *task;
    enum xstate_status status = online_cpu(state, cpu_index, &cpu);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = resolve_task(state, handle, &task);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_LIVE) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (cpu->loaded_task == XSTATE_INVALID_INDEX) {
        return XSTATE_STATUS_TASK_NOT_LOADED;
    }
    if (cpu->loaded_task != handle.index ||
        cpu->loaded_generation != handle.generation ||
        task->owner_cpu != cpu_index) {
        return XSTATE_STATUS_OWNER_MISMATCH;
    }
    if (task->save_count == UINT64_MAX) {
        state->poisoned = UINT8_C(1);
        return XSTATE_STATUS_COUNTER_EXHAUSTED;
    }

    ++task->save_count;
    task->owner_cpu = XSTATE_INVALID_INDEX;
    cpu->loaded_task = XSTATE_INVALID_INDEX;
    cpu->loaded_generation = 0U;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_record_restore(
    struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_task_handle handle
)
{
    struct xstate_core_cpu *cpu;
    struct xstate_core_task *task;
    enum xstate_status status = online_cpu(state, cpu_index, &cpu);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = resolve_task(state, handle, &task);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_LIVE) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (task->image_ready == 0U) {
        return XSTATE_STATUS_IMAGE_NOT_READY;
    }
    if (cpu->loaded_task != XSTATE_INVALID_INDEX) {
        return XSTATE_STATUS_CPU_ALREADY_LOADED;
    }
    if (task->owner_cpu != XSTATE_INVALID_INDEX) {
        return XSTATE_STATUS_TASK_ALREADY_LOADED;
    }
    if (task->restore_count == UINT64_MAX) {
        state->poisoned = UINT8_C(1);
        return XSTATE_STATUS_COUNTER_EXHAUSTED;
    }

    ++task->restore_count;
    task->owner_cpu = cpu_index;
    cpu->loaded_task = handle.index;
    cpu->loaded_generation = handle.generation;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_task_begin_destroy(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_LIVE) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (task->owner_cpu != XSTATE_INVALID_INDEX) {
        return XSTATE_STATUS_TASK_ALREADY_LOADED;
    }
    task->state = (uint8_t)XSTATE_TASK_DYING;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_task_abort_destroy(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_DYING) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    task->state = (uint8_t)XSTATE_TASK_LIVE;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_task_reap(
    struct xstate_core_state *state,
    struct xstate_task_handle handle
)
{
    struct xstate_core_task *task;
    enum xstate_status status = mutable_task(state, handle, &task);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (task->state != (uint8_t)XSTATE_TASK_DYING) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    release_task_slot(state, task);
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_require_features(
    struct xstate_core_state *state,
    uint64_t required_mask
)
{
    enum xstate_status status = mutation_gate(state);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if ((required_mask & ~state->layout.enabled_mask) != 0U) {
        state->poisoned = UINT8_C(1);
        return XSTATE_STATUS_UNSUPPORTED_FEATURE;
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_unexpected_nm(
    struct xstate_core_state *state,
    uint32_t cpu_index
)
{
    struct xstate_core_cpu *cpu;
    enum xstate_status status = online_cpu(state, cpu_index, &cpu);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    (void)cpu;
    if (state->unexpected_nm_count == UINT32_MAX) {
        state->poisoned = UINT8_C(1);
        return XSTATE_STATUS_COUNTER_EXHAUSTED;
    }
    ++state->unexpected_nm_count;
    state->poisoned = UINT8_C(1);
    return XSTATE_STATUS_UNEXPECTED_NM;
}

enum xstate_status xstate_core_task_snapshot(
    const struct xstate_core_state *state,
    struct xstate_task_handle handle,
    struct xstate_task_snapshot *snapshot
)
{
    const struct xstate_core_task *task;
    enum xstate_status status;

    if (state == NULL || snapshot == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    snapshot->state = XSTATE_TASK_FREE;
    snapshot->generation = 0U;
    snapshot->owner_cpu = XSTATE_INVALID_INDEX;
    snapshot->save_count = 0U;
    snapshot->restore_count = 0U;
    snapshot->image_ready = false;
    status = xstate_core_validate(state);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = resolve_const_task(state, handle, &task);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    snapshot->state = (enum xstate_task_state)task->state;
    snapshot->generation = task->generation;
    snapshot->owner_cpu = task->owner_cpu;
    snapshot->save_count = task->save_count;
    snapshot->restore_count = task->restore_count;
    snapshot->image_ready = task->image_ready == UINT8_C(1);
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_core_cpu_snapshot(
    const struct xstate_core_state *state,
    uint32_t cpu_index,
    struct xstate_cpu_snapshot *snapshot
)
{
    enum xstate_status status;

    if (state == NULL || snapshot == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    snapshot->loaded_task = XSTATE_INVALID_INDEX;
    snapshot->loaded_generation = 0U;
    snapshot->online = false;
    status = xstate_core_validate(state);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (cpu_index >= XSTATE_CPU_LIMIT) {
        return XSTATE_STATUS_INVALID_CPU;
    }
    snapshot->loaded_task = state->cpus[cpu_index].loaded_task;
    snapshot->loaded_generation = state->cpus[cpu_index].loaded_generation;
    snapshot->online = state->cpus[cpu_index].online == UINT8_C(1);
    return XSTATE_STATUS_OK;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/cpu.h>
#include <zenith/xstate.h>

#include "xstate_core.h"
#include "xstate_runtime.h"

#define XSTATE_CPUID_FEATURES UINT32_C(1)
#define XSTATE_CPUID_LAYOUT UINT32_C(0xD)
#define XSTATE_CPUID_ECX_XSAVE (UINT32_C(1) << 26U)
#define XSTATE_CPUID_ECX_OSXSAVE (UINT32_C(1) << 27U)
#define XSTATE_CPUID_ECX_AVX (UINT32_C(1) << 28U)
#define XSTATE_CPUID_EDX_FPU (UINT32_C(1) << 0U)
#define XSTATE_CPUID_EDX_FXSR (UINT32_C(1) << 24U)
#define XSTATE_CPUID_EDX_SSE (UINT32_C(1) << 25U)
#define XSTATE_CPUID_EDX_SSE2 (UINT32_C(1) << 26U)
#define XSTATE_CR0_MP (UINT64_C(1) << 1U)
#define XSTATE_CR0_EM (UINT64_C(1) << 2U)
#define XSTATE_CR0_TS (UINT64_C(1) << 3U)
#define XSTATE_CR0_NE (UINT64_C(1) << 5U)
#define XSTATE_CR4_OSFXSR (UINT64_C(1) << 9U)
#define XSTATE_CR4_OSXMMEXCPT (UINT64_C(1) << 10U)
#define XSTATE_CR4_OSXSAVE (UINT64_C(1) << 18U)
#define XSTATE_BOOT_CPU UINT32_C(0)

struct xstate_runtime_task {
    struct xstate_task_handle handle;
    bool image_initialized;
};

uint64_t xstate_arch_read_cr4(void);
void xstate_arch_write_cr4(uint64_t value);
uint64_t xstate_arch_xgetbv(uint32_t register_index);
void xstate_arch_xsetbv(uint32_t register_index, uint64_t value);
void xstate_arch_save(void *area, uint64_t mask);
void xstate_arch_restore(const void *area, uint64_t mask);

static struct xstate_core_state runtime_core;
static struct xstate_runtime_task runtime_tasks[XSTATE_TASK_LIMIT];
static _Alignas(XSTATE_AREA_ALIGNMENT)
    uint8_t bootstrap_image[XSTATE_MAXIMUM_AREA_SIZE];
static _Alignas(XSTATE_AREA_ALIGNMENT)
    uint8_t task_images[XSTATE_TASK_LIMIT][XSTATE_MAXIMUM_AREA_SIZE];
static uint64_t bootstrap_save_count;
static uint64_t bootstrap_restore_count;
static bool bootstrap_loaded;
static bool runtime_initialized;
static bool runtime_poisoned;
static bool switch_in_progress;

_Static_assert(XSTATE_TASK_LIMIT == UINT32_C(16),
    "XSAVE runtime must preserve all 16 dynamic task slots");
_Static_assert(sizeof(task_images[0]) == XSTATE_MAXIMUM_AREA_SIZE,
    "XSAVE task image stride must preserve alignment");
_Static_assert(
    XSTATE_MAXIMUM_AREA_SIZE % XSTATE_AREA_ALIGNMENT == 0U,
    "XSAVE image stride must preserve 64-byte alignment"
);

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static void runtime_task_clear(struct xstate_runtime_task *task)
{
    task->handle.index = XSTATE_INVALID_INDEX;
    task->handle.generation = 0U;
    task->image_initialized = false;
}

static bool handles_equal(
    struct xstate_task_handle left,
    struct xstate_task_handle right
)
{
    return left.index == right.index && left.generation == right.generation;
}

static uint64_t image_u64(const uint8_t *image, size_t offset)
{
    uint64_t value = 0U;

    for (size_t byte = 0U; byte < sizeof(value); ++byte) {
        value |= (uint64_t)image[offset + byte] << (byte * 8U);
    }
    return value;
}

static uint32_t image_u32(const uint8_t *image, size_t offset)
{
    uint32_t value = 0U;

    for (size_t byte = 0U; byte < sizeof(value); ++byte) {
        value |= (uint32_t)image[offset + byte] << (byte * 8U);
    }
    return value;
}

static bool image_header_is_valid(const uint8_t *image, uint64_t enabled_mask)
{
    const size_t header = XSTATE_LEGACY_AREA_SIZE;
    const uint64_t image_mask = image_u64(image, header);

    if ((image_mask & ~enabled_mask) != 0U ||
        image_u64(image, header + sizeof(uint64_t)) != 0U) {
        return false;
    }
    for (size_t offset = header + 2U * sizeof(uint64_t);
         offset < XSTATE_MINIMUM_AREA_SIZE;
         ++offset) {
        if (image[offset] != 0U) {
            return false;
        }
    }
    if ((image_mask & XSTATE_SSE_MASK) != 0U) {
        uint32_t mxcsr_mask = image_u32(image, 28U);

        if ((mxcsr_mask & ~UINT32_C(0x0000FFFF)) != 0U) {
            return false;
        }
        if (mxcsr_mask == 0U) {
            mxcsr_mask = UINT32_C(0x0000FFBF);
        }
        if ((image_u32(image, 24U) & ~mxcsr_mask) != 0U) {
            return false;
        }
    }
    return true;
}

static enum xstate_status runtime_gate(void)
{
    if (!runtime_initialized) {
        return XSTATE_STATUS_NOT_INITIALIZED;
    }
    if (runtime_poisoned || runtime_core.poisoned != 0U) {
        return XSTATE_STATUS_POISONED;
    }
    return XSTATE_STATUS_OK;
}

static enum xstate_status probe_layout(
    struct xstate_layout *layout,
    uint64_t *original_cr0,
    uint64_t *original_cr4,
    uint64_t *original_xcr0
)
{
    struct cpu_cpuid_result highest;
    struct cpu_cpuid_result features;
    struct cpu_cpuid_result enabled;
    struct cpu_cpuid_result avx;
    struct cpu_cpuid_result refreshed;
    struct xstate_layout_probe probe = {0};
    uint64_t supported_mask;
    uint64_t target_xcr0;

    cpu_cpuid(0U, 0U, &highest);
    if (highest.eax < XSTATE_CPUID_LAYOUT) {
        return XSTATE_STATUS_CPUID_LEAF_UNAVAILABLE;
    }

    cpu_cpuid(XSTATE_CPUID_FEATURES, 0U, &features);
    if ((features.ecx & XSTATE_CPUID_ECX_XSAVE) == 0U) {
        return XSTATE_STATUS_XSAVE_UNAVAILABLE;
    }
    if ((features.edx & (XSTATE_CPUID_EDX_FPU | XSTATE_CPUID_EDX_FXSR |
            XSTATE_CPUID_EDX_SSE | XSTATE_CPUID_EDX_SSE2)) !=
            (XSTATE_CPUID_EDX_FPU | XSTATE_CPUID_EDX_FXSR |
                XSTATE_CPUID_EDX_SSE | XSTATE_CPUID_EDX_SSE2)) {
        return XSTATE_STATUS_UNSUPPORTED_FEATURE;
    }

    cpu_cpuid(XSTATE_CPUID_LAYOUT, 0U, &enabled);
    supported_mask = (uint64_t)enabled.eax |
        ((uint64_t)enabled.edx << 32U);
    target_xcr0 = XSTATE_X87_MASK | XSTATE_SSE_MASK;
    if ((supported_mask & XSTATE_AVX_MASK) != 0U &&
        (features.ecx & XSTATE_CPUID_ECX_AVX) != 0U) {
        target_xcr0 |= XSTATE_AVX_MASK;
    }
    if ((supported_mask & target_xcr0) != target_xcr0) {
        return XSTATE_STATUS_UNSUPPORTED_FEATURE;
    }

    *original_cr0 = cpu_read_cr0();
    *original_cr4 = xstate_arch_read_cr4();
    cpu_write_cr0((*original_cr0 | XSTATE_CR0_MP | XSTATE_CR0_NE) &
        ~(XSTATE_CR0_EM | XSTATE_CR0_TS));
    xstate_arch_write_cr4(*original_cr4 | XSTATE_CR4_OSFXSR |
        XSTATE_CR4_OSXMMEXCPT | XSTATE_CR4_OSXSAVE);

    if ((cpu_read_cr0() & (XSTATE_CR0_MP | XSTATE_CR0_NE)) !=
            (XSTATE_CR0_MP | XSTATE_CR0_NE) ||
        (cpu_read_cr0() & (XSTATE_CR0_EM | XSTATE_CR0_TS)) != 0U ||
        (xstate_arch_read_cr4() & (XSTATE_CR4_OSFXSR |
            XSTATE_CR4_OSXMMEXCPT | XSTATE_CR4_OSXSAVE)) !=
                (XSTATE_CR4_OSFXSR | XSTATE_CR4_OSXMMEXCPT |
                    XSTATE_CR4_OSXSAVE)) {
        xstate_arch_write_cr4(*original_cr4);
        cpu_write_cr0(*original_cr0);
        return XSTATE_STATUS_OSXSAVE_UNAVAILABLE;
    }

    cpu_cpuid(XSTATE_CPUID_FEATURES, 0U, &refreshed);
    if ((refreshed.ecx & XSTATE_CPUID_ECX_OSXSAVE) == 0U) {
        xstate_arch_write_cr4(*original_cr4);
        cpu_write_cr0(*original_cr0);
        return XSTATE_STATUS_OSXSAVE_UNAVAILABLE;
    }

    *original_xcr0 = xstate_arch_xgetbv(0U);
    xstate_arch_xsetbv(0U, target_xcr0);
    if (xstate_arch_xgetbv(0U) != target_xcr0) {
        xstate_arch_xsetbv(0U, *original_xcr0);
        xstate_arch_write_cr4(*original_cr4);
        cpu_write_cr0(*original_cr0);
        return XSTATE_STATUS_INVALID_XCR0;
    }

    cpu_cpuid(XSTATE_CPUID_LAYOUT, 0U, &enabled);
    cpu_cpuid(XSTATE_CPUID_LAYOUT, 2U, &avx);
    probe.cpuid_xcr0_mask = supported_mask;
    probe.xcr0 = target_xcr0;
    probe.enabled_area_size = enabled.ebx;
    probe.maximum_area_size = enabled.ecx;
    if ((supported_mask & XSTATE_AVX_MASK) != 0U) {
        probe.avx_component_size = avx.eax;
        probe.avx_component_offset = avx.ebx;
    }
    probe.xsave = UINT8_C(1);
    probe.osxsave = UINT8_C(1);
    probe.leaf_d = UINT8_C(1);
    {
        enum xstate_status status = xstate_layout_derive(&probe, layout);

        if (status != XSTATE_STATUS_OK) {
            xstate_arch_xsetbv(0U, *original_xcr0);
            xstate_arch_write_cr4(*original_cr4);
            cpu_write_cr0(*original_cr0);
        }
        return status;
    }
}

enum xstate_status xstate_runtime_initialize(void)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct xstate_layout layout;
    uint64_t original_cr0 = 0U;
    uint64_t original_cr4 = 0U;
    uint64_t original_xcr0 = 0U;
    enum xstate_status status;

    cpu_interrupt_disable();
    if (runtime_initialized) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return XSTATE_STATUS_ALREADY_INITIALIZED;
    }

    status = probe_layout(
        &layout,
        &original_cr0,
        &original_cr4,
        &original_xcr0
    );
    if (status != XSTATE_STATUS_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status;
    }

    xstate_core_clear(&runtime_core);
    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        runtime_task_clear(&runtime_tasks[index]);
        bytes_zero(task_images[index], sizeof(task_images[index]));
    }
    bytes_zero(bootstrap_image, sizeof(bootstrap_image));
    bootstrap_save_count = 0U;
    bootstrap_restore_count = 0U;
    bootstrap_loaded = true;
    runtime_poisoned = false;
    switch_in_progress = false;

    status = xstate_core_initialize(&runtime_core, &layout, XSTATE_BOOT_CPU);
    if (status != XSTATE_STATUS_OK ||
        xstate_area_validate(
            &layout,
            (uintptr_t)(void *)bootstrap_image,
            sizeof(bootstrap_image)
        ) != XSTATE_STATUS_OK) {
        xstate_arch_xsetbv(0U, original_xcr0);
        xstate_arch_write_cr4(original_cr4);
        cpu_write_cr0(original_cr0);
        xstate_core_clear(&runtime_core);
        bootstrap_loaded = false;
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status == XSTATE_STATUS_OK
            ? XSTATE_STATUS_MISALIGNED_AREA
            : status;
    }

    xstate_arch_save(bootstrap_image, layout.enabled_mask);
    runtime_initialized = true;
    status = xstate_runtime_validate();
    if (status != XSTATE_STATUS_OK) {
        runtime_initialized = false;
        xstate_arch_xsetbv(0U, original_xcr0);
        xstate_arch_write_cr4(original_cr4);
        cpu_write_cr0(original_cr0);
        xstate_core_clear(&runtime_core);
        bootstrap_loaded = false;
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status;
    }
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return XSTATE_STATUS_OK;
}

bool xstate_runtime_is_initialized(void)
{
    return runtime_initialized && !runtime_poisoned;
}

static enum xstate_status runtime_validate_unguarded(void)
{
    struct xstate_cpu_snapshot cpu;
    uint32_t observed_live = 0U;
    enum xstate_status status = runtime_gate();

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = xstate_core_validate(&runtime_core);
    if (status != XSTATE_STATUS_OK || switch_in_progress ||
        (cpu_read_cr0() & (XSTATE_CR0_MP | XSTATE_CR0_NE)) !=
            (XSTATE_CR0_MP | XSTATE_CR0_NE) ||
        (cpu_read_cr0() & (XSTATE_CR0_EM | XSTATE_CR0_TS)) != 0U ||
        (xstate_arch_read_cr4() & (XSTATE_CR4_OSFXSR |
            XSTATE_CR4_OSXMMEXCPT | XSTATE_CR4_OSXSAVE)) !=
                (XSTATE_CR4_OSFXSR | XSTATE_CR4_OSXMMEXCPT |
                    XSTATE_CR4_OSXSAVE) ||
        xstate_arch_xgetbv(0U) != runtime_core.layout.enabled_mask ||
        xstate_layout_validate(&runtime_core.layout) != XSTATE_STATUS_OK ||
        xstate_area_validate(
            &runtime_core.layout,
            (uintptr_t)(void *)bootstrap_image,
            sizeof(bootstrap_image)
        ) != XSTATE_STATUS_OK ||
        xstate_core_cpu_snapshot(&runtime_core, XSTATE_BOOT_CPU, &cpu) !=
            XSTATE_STATUS_OK) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return XSTATE_STATUS_CORRUPTED;
    }

    for (size_t index = 0U; index < XSTATE_TASK_LIMIT; ++index) {
        const struct xstate_runtime_task *task = &runtime_tasks[index];
        const enum xstate_task_state task_state =
            (enum xstate_task_state)runtime_core.tasks[index].state;

        if (task_state == XSTATE_TASK_LIVE) {
            if (!task->image_initialized || task->handle.index != index ||
                task->handle.generation !=
                    runtime_core.tasks[index].generation ||
                xstate_area_validate(
                    &runtime_core.layout,
                    (uintptr_t)(void *)task_images[index],
                    sizeof(task_images[index])
                ) != XSTATE_STATUS_OK) {
                runtime_poisoned = true;
                runtime_core.poisoned = UINT8_C(1);
                return XSTATE_STATUS_CORRUPTED;
            }
            ++observed_live;
        } else if (task->image_initialized ||
            task->handle.index != XSTATE_INVALID_INDEX ||
            task->handle.generation != 0U) {
            runtime_poisoned = true;
            runtime_core.poisoned = UINT8_C(1);
            return XSTATE_STATUS_CORRUPTED;
        }
    }

    if (observed_live != runtime_core.active_tasks ||
        bootstrap_loaded == (cpu.loaded_task != XSTATE_INVALID_INDEX) ||
        (!bootstrap_loaded && cpu.loaded_task >= XSTATE_TASK_LIMIT) ||
        (bootstrap_loaded &&
            bootstrap_restore_count != bootstrap_save_count) ||
        (!bootstrap_loaded &&
            (bootstrap_save_count == 0U ||
             bootstrap_restore_count != bootstrap_save_count - 1U))) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return XSTATE_STATUS_CORRUPTED;
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_runtime_validate(void)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum xstate_status status;

    cpu_interrupt_disable();
    status = runtime_validate_unguarded();
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

enum xstate_status xstate_runtime_task_create(
    struct xstate_task_handle *handle
)
{
    struct xstate_task_handle candidate;
    enum xstate_status status;

    if (handle == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    handle->index = XSTATE_INVALID_INDEX;
    handle->generation = 0U;
    status = xstate_runtime_validate();
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = xstate_core_task_create(&runtime_core, &candidate);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (candidate.index >= XSTATE_TASK_LIMIT ||
        runtime_tasks[candidate.index].image_initialized) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return XSTATE_STATUS_CORRUPTED;
    }

    bytes_zero(task_images[candidate.index], sizeof(task_images[0]));
    runtime_tasks[candidate.index].handle = candidate;
    runtime_tasks[candidate.index].image_initialized = true;
    status = xstate_core_task_mark_image_ready(&runtime_core, candidate);
    if (status == XSTATE_STATUS_OK) {
        status = xstate_core_task_publish(&runtime_core, candidate);
    }
    if (status != XSTATE_STATUS_OK) {
        if (runtime_core.tasks[candidate.index].state ==
                (uint8_t)XSTATE_TASK_BUILDING) {
            (void)xstate_core_task_abort(&runtime_core, candidate);
        }
        runtime_task_clear(&runtime_tasks[candidate.index]);
        bytes_zero(task_images[candidate.index], sizeof(task_images[0]));
        return status;
    }
    if (xstate_runtime_validate() != XSTATE_STATUS_OK) {
        return XSTATE_STATUS_CORRUPTED;
    }
    *handle = candidate;
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_runtime_task_destroy(
    struct xstate_task_handle handle
)
{
    enum xstate_status status = xstate_runtime_task_validate(handle, false);

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = xstate_core_task_begin_destroy(&runtime_core, handle);
    if (status == XSTATE_STATUS_OK) {
        status = xstate_core_task_reap(&runtime_core, handle);
    }
    if (status != XSTATE_STATUS_OK) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return status;
    }
    runtime_task_clear(&runtime_tasks[handle.index]);
    bytes_zero(task_images[handle.index], sizeof(task_images[0]));
    return xstate_runtime_validate();
}

enum xstate_status xstate_runtime_task_validate(
    struct xstate_task_handle handle,
    bool expected_loaded
)
{
    struct xstate_task_snapshot task;
    enum xstate_status status = xstate_runtime_validate();

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = xstate_core_task_snapshot(&runtime_core, handle, &task);
    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (handle.index >= XSTATE_TASK_LIMIT ||
        !handles_equal(runtime_tasks[handle.index].handle, handle) ||
        !runtime_tasks[handle.index].image_initialized ||
        task.state != XSTATE_TASK_LIVE ||
        expected_loaded != (task.owner_cpu == XSTATE_BOOT_CPU)) {
        return XSTATE_STATUS_OWNER_MISMATCH;
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_runtime_switch(
    bool previous_bootstrap,
    struct xstate_task_handle previous,
    bool next_bootstrap,
    struct xstate_task_handle next
)
{
    struct xstate_task_snapshot previous_snapshot;
    struct xstate_task_snapshot next_snapshot;
    enum xstate_status status = xstate_runtime_validate();

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    if (cpu_interrupts_enabled()) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (previous_bootstrap == next_bootstrap &&
        (previous_bootstrap || handles_equal(previous, next))) {
        return XSTATE_STATUS_INVALID_STATE;
    }
    if (previous_bootstrap) {
        if (!bootstrap_loaded || bootstrap_save_count == UINT64_MAX) {
            runtime_poisoned = true;
            return XSTATE_STATUS_COUNTER_EXHAUSTED;
        }
    } else {
        status = xstate_core_task_snapshot(
            &runtime_core,
            previous,
            &previous_snapshot
        );
        if (status != XSTATE_STATUS_OK ||
            previous_snapshot.owner_cpu != XSTATE_BOOT_CPU ||
            previous_snapshot.save_count == UINT64_MAX) {
            runtime_poisoned = true;
            runtime_core.poisoned = UINT8_C(1);
            return status == XSTATE_STATUS_OK
                ? XSTATE_STATUS_OWNER_MISMATCH
                : status;
        }
    }
    if (next_bootstrap) {
        if (bootstrap_loaded || bootstrap_restore_count == UINT64_MAX) {
            runtime_poisoned = true;
            return XSTATE_STATUS_COUNTER_EXHAUSTED;
        }
    } else {
        status = xstate_core_task_snapshot(&runtime_core, next, &next_snapshot);
        if (status != XSTATE_STATUS_OK ||
            next_snapshot.owner_cpu != XSTATE_INVALID_INDEX ||
            next_snapshot.restore_count == UINT64_MAX) {
            runtime_poisoned = true;
            runtime_core.poisoned = UINT8_C(1);
            return status == XSTATE_STATUS_OK
                ? XSTATE_STATUS_OWNER_MISMATCH
                : status;
        }
    }

    if (!image_header_is_valid(
            next_bootstrap ? bootstrap_image : task_images[next.index],
            runtime_core.layout.enabled_mask
        )) {
        runtime_poisoned = true;
        runtime_core.poisoned = UINT8_C(1);
        return XSTATE_STATUS_CORRUPTED;
    }

    switch_in_progress = true;
    if (previous_bootstrap) {
        xstate_arch_save(bootstrap_image, runtime_core.layout.enabled_mask);
        ++bootstrap_save_count;
        bootstrap_loaded = false;
    } else {
        xstate_arch_save(
            task_images[previous.index],
            runtime_core.layout.enabled_mask
        );
        status = xstate_core_record_save(
            &runtime_core,
            XSTATE_BOOT_CPU,
            previous
        );
        if (status != XSTATE_STATUS_OK) {
            switch_in_progress = false;
            runtime_poisoned = true;
            runtime_core.poisoned = UINT8_C(1);
            return status;
        }
    }

    if (next_bootstrap) {
        xstate_arch_restore(bootstrap_image, runtime_core.layout.enabled_mask);
        ++bootstrap_restore_count;
        bootstrap_loaded = true;
    } else {
        xstate_arch_restore(
            task_images[next.index],
            runtime_core.layout.enabled_mask
        );
        status = xstate_core_record_restore(
            &runtime_core,
            XSTATE_BOOT_CPU,
            next
        );
        if (status != XSTATE_STATUS_OK) {
            switch_in_progress = false;
            runtime_poisoned = true;
            runtime_core.poisoned = UINT8_C(1);
            return status;
        }
    }
    switch_in_progress = false;
    return runtime_validate_unguarded();
}

enum xstate_status xstate_runtime_snapshot(
    struct xstate_runtime_snapshot *snapshot
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    struct xstate_cpu_snapshot cpu;
    enum xstate_status status;

    if (snapshot == NULL) {
        return XSTATE_STATUS_NULL_ARGUMENT;
    }
    cpu_interrupt_disable();
    bytes_zero(snapshot, sizeof(*snapshot));
    snapshot->loaded_task = XSTATE_INVALID_INDEX;
    status = runtime_validate_unguarded();
    if (status != XSTATE_STATUS_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status;
    }
    status = xstate_core_cpu_snapshot(&runtime_core, XSTATE_BOOT_CPU, &cpu);
    if (status != XSTATE_STATUS_OK) {
        if (interrupts_were_enabled) {
            cpu_interrupt_enable();
        }
        return status;
    }
    snapshot->layout = runtime_core.layout;
    snapshot->live_tasks = runtime_core.active_tasks;
    snapshot->loaded_task = cpu.loaded_task;
    snapshot->loaded_generation = cpu.loaded_generation;
    snapshot->bootstrap_save_count = bootstrap_save_count;
    snapshot->bootstrap_restore_count = bootstrap_restore_count;
    snapshot->bootstrap_loaded = bootstrap_loaded;
    snapshot->initialized = runtime_initialized;
    snapshot->poisoned = runtime_poisoned;
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return XSTATE_STATUS_OK;
}

enum xstate_status xstate_runtime_unexpected_nm(void)
{
    enum xstate_status status = runtime_gate();

    if (status != XSTATE_STATUS_OK) {
        return status;
    }
    status = xstate_core_unexpected_nm(&runtime_core, XSTATE_BOOT_CPU);
    runtime_poisoned = true;
    return status;
}

const char *xstate_status_string(enum xstate_status status)
{
    switch (status) {
    case XSTATE_STATUS_OK: return "ok";
    case XSTATE_STATUS_NULL_ARGUMENT: return "null XSAVE argument";
    case XSTATE_STATUS_ALREADY_INITIALIZED: return "XSAVE initialized twice";
    case XSTATE_STATUS_NOT_INITIALIZED: return "XSAVE is not initialized";
    case XSTATE_STATUS_POISONED: return "XSAVE runtime is poisoned";
    case XSTATE_STATUS_XSAVE_UNAVAILABLE: return "XSAVE is unavailable";
    case XSTATE_STATUS_OSXSAVE_UNAVAILABLE: return "OSXSAVE is unavailable";
    case XSTATE_STATUS_CPUID_LEAF_UNAVAILABLE:
        return "CPUID leaf 0xD is unavailable";
    case XSTATE_STATUS_INVALID_XCR0: return "XCR0 is invalid";
    case XSTATE_STATUS_UNSUPPORTED_FEATURE:
        return "required XSAVE feature is unsupported";
    case XSTATE_STATUS_INVALID_AREA_SIZE: return "XSAVE area size is invalid";
    case XSTATE_STATUS_INVALID_COMPONENT:
        return "XSAVE component layout is invalid";
    case XSTATE_STATUS_MISALIGNED_AREA: return "XSAVE area is misaligned";
    case XSTATE_STATUS_AREA_TOO_SMALL: return "XSAVE area is too small";
    case XSTATE_STATUS_NO_TASK_SLOT: return "XSAVE task slots are exhausted";
    case XSTATE_STATUS_INVALID_HANDLE: return "XSAVE handle is invalid";
    case XSTATE_STATUS_STALE_HANDLE: return "XSAVE handle is stale";
    case XSTATE_STATUS_GENERATION_EXHAUSTED:
        return "XSAVE task generation is exhausted";
    case XSTATE_STATUS_INVALID_STATE: return "XSAVE state is invalid";
    case XSTATE_STATUS_IMAGE_NOT_READY: return "XSAVE image is not ready";
    case XSTATE_STATUS_IMAGE_ALREADY_READY:
        return "XSAVE image is already ready";
    case XSTATE_STATUS_CPU_ALREADY_LOADED:
        return "CPU already owns an XSAVE image";
    case XSTATE_STATUS_TASK_ALREADY_LOADED:
        return "XSAVE task is already loaded";
    case XSTATE_STATUS_TASK_NOT_LOADED: return "XSAVE task is not loaded";
    case XSTATE_STATUS_OWNER_MISMATCH: return "XSAVE owner mismatch";
    case XSTATE_STATUS_COUNTER_EXHAUSTED:
        return "XSAVE counter is exhausted";
    case XSTATE_STATUS_UNEXPECTED_NM: return "unexpected device-not-available";
    case XSTATE_STATUS_INVALID_CPU:
    case XSTATE_STATUS_CPU_ALREADY_ONLINE:
    case XSTATE_STATUS_CPU_OFFLINE:
    case XSTATE_STATUS_CPU_BUSY:
    case XSTATE_STATUS_LAST_CPU:
    case XSTATE_STATUS_CORRUPTED:
    default:
        return "XSAVE runtime metadata is corrupted";
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/cpu.h>
#include <zenith/percpu.h>

#include "percpu_core.h"

struct percpu_runtime_slot {
    volatile uint32_t apic_id;
    volatile uint32_t generation;
    volatile uint32_t online;
};

static struct percpu_core_state registry;
static struct percpu_runtime_slot published[PERCPU_CPU_LIMIT];
static volatile uint32_t registry_gate;
static volatile uint32_t runtime_initialized;
static volatile uint32_t runtime_poisoned;

static uint32_t atomic_load_acquire(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void atomic_store_release(volatile uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static void cpu_relax(void)
{
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

static void gate_lock(void)
{
    uint32_t expected;

    for (;;) {
        expected = 0U;
        if (__atomic_compare_exchange_n(
                &registry_gate, &expected, UINT32_C(1), false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        cpu_relax();
    }
}

static void gate_unlock(void)
{
    atomic_store_release(&registry_gate, 0U);
}

static void restore_interrupt_state(bool enabled)
{
    if (enabled) {
        cpu_interrupt_enable();
    } else {
        cpu_interrupt_disable();
    }
}

static uint32_t current_apic_id(void)
{
    struct cpu_cpuid_result result;

    cpu_cpuid(UINT32_C(1), 0U, &result);
    return result.ebx >> 24U;
}

static void clear_publication(void)
{
    for (size_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        atomic_store_release(&published[index].online, 0U);
        atomic_store_release(
            &published[index].apic_id, PERCPU_INVALID_APIC_ID
        );
        atomic_store_release(&published[index].generation, 0U);
    }
}

static void publish_cpu(uint32_t cpu_index)
{
    atomic_store_release(
        &published[cpu_index].apic_id,
        registry.cpus[cpu_index].apic_id
    );
    atomic_store_release(
        &published[cpu_index].generation,
        registry.cpus[cpu_index].generation
    );
    atomic_store_release(&published[cpu_index].online, UINT32_C(1));
}

bool percpu_runtime_ready(void)
{
    return atomic_load_acquire(&runtime_initialized) == UINT32_C(1) &&
        atomic_load_acquire(&runtime_poisoned) == 0U;
}

static enum percpu_status unavailable_status(void)
{
    return atomic_load_acquire(&runtime_initialized) == UINT32_C(1) ?
        PERCPU_STATUS_POISONED : PERCPU_STATUS_NOT_INITIALIZED;
}

enum percpu_status percpu_runtime_initialize(
    uint32_t boot_cpu_index,
    uint32_t boot_apic_id
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum percpu_status status;

    cpu_interrupt_disable();
    gate_lock();
    if (atomic_load_acquire(&runtime_initialized) == UINT32_C(1)) {
        status = PERCPU_STATUS_ALREADY_INITIALIZED;
    } else {
        percpu_core_clear(&registry);
        clear_publication();
        status = percpu_core_initialize(
            &registry, boot_cpu_index, boot_apic_id
        );
        if (status == PERCPU_STATUS_OK) {
            publish_cpu(boot_cpu_index);
            atomic_store_release(&runtime_poisoned, 0U);
            atomic_store_release(&runtime_initialized, UINT32_C(1));
        }
    }
    gate_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum percpu_status percpu_runtime_initialize_current(uint32_t boot_cpu_index)
{
    return percpu_runtime_initialize(boot_cpu_index, current_apic_id());
}

enum percpu_status percpu_runtime_register_cpu(
    uint32_t cpu_index,
    uint32_t apic_id,
    struct percpu_handle *handle
)
{
    bool interrupts_were_enabled;
    enum percpu_status status;

    if (handle == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    handle->cpu_index = PERCPU_INVALID_CPU;
    handle->generation = 0U;
    if (!percpu_runtime_ready()) {
        return unavailable_status();
    }

    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    gate_lock();
    status = percpu_core_cpu_online(&registry, cpu_index, apic_id);
    if (status == PERCPU_STATUS_OK) {
        status = percpu_core_handle(&registry, cpu_index, handle);
        if (status == PERCPU_STATUS_OK) {
            publish_cpu(cpu_index);
        } else {
            registry.poisoned = UINT8_C(1);
            atomic_store_release(&runtime_poisoned, UINT32_C(1));
            handle->cpu_index = PERCPU_INVALID_CPU;
            handle->generation = 0U;
            status = PERCPU_STATUS_CORRUPTED;
        }
    }
    if (status == PERCPU_STATUS_POISONED ||
        status == PERCPU_STATUS_CORRUPTED ||
        status == PERCPU_STATUS_GENERATION_EXHAUSTED) {
        atomic_store_release(&runtime_poisoned, UINT32_C(1));
    }
    gate_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum percpu_status percpu_runtime_current_cpu(uint32_t *cpu_index)
{
    uint32_t apic_id;

    if (cpu_index == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    *cpu_index = PERCPU_INVALID_CPU;
    if (!percpu_runtime_ready()) {
        return unavailable_status();
    }
    apic_id = current_apic_id();

    for (uint32_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
        if (atomic_load_acquire(&published[index].online) == UINT32_C(1) &&
            atomic_load_acquire(&published[index].apic_id) == apic_id) {
            if (!percpu_runtime_ready()) {
                return unavailable_status();
            }
            *cpu_index = index;
            return PERCPU_STATUS_OK;
        }
    }
    return PERCPU_STATUS_CPU_OFFLINE;
}

enum percpu_status percpu_runtime_current_handle(struct percpu_handle *handle)
{
    uint32_t cpu_index;
    enum percpu_status status;

    if (handle == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    handle->cpu_index = PERCPU_INVALID_CPU;
    handle->generation = 0U;
    status = percpu_runtime_current_cpu(&cpu_index);
    if (status != PERCPU_STATUS_OK) {
        return status;
    }
    handle->cpu_index = cpu_index;
    handle->generation = atomic_load_acquire(
        &published[cpu_index].generation
    );
    if (handle->generation == 0U) {
        handle->cpu_index = PERCPU_INVALID_CPU;
        return PERCPU_STATUS_CORRUPTED;
    }
    if (!percpu_runtime_ready()) {
        handle->cpu_index = PERCPU_INVALID_CPU;
        handle->generation = 0U;
        return unavailable_status();
    }
    return PERCPU_STATUS_OK;
}

enum percpu_status percpu_runtime_resolve(
    const struct percpu_handle *handle,
    uint32_t *cpu_index
)
{
    bool interrupts_were_enabled;
    enum percpu_status status;

    if (cpu_index == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    *cpu_index = PERCPU_INVALID_CPU;
    if (handle == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    if (!percpu_runtime_ready()) {
        return unavailable_status();
    }
    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    gate_lock();
    status = percpu_core_resolve(&registry, handle, cpu_index);
    gate_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum percpu_status percpu_runtime_snapshot(
    uint32_t cpu_index,
    struct percpu_snapshot *snapshot
)
{
    bool interrupts_were_enabled;
    enum percpu_status status;

    if (snapshot == NULL) {
        return PERCPU_STATUS_NULL_ARGUMENT;
    }
    snapshot->apic_id = PERCPU_INVALID_APIC_ID;
    snapshot->generation = 0U;
    snapshot->online = false;
    snapshot->boot_cpu = false;
    if (!percpu_runtime_ready()) {
        return unavailable_status();
    }
    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    gate_lock();
    status = percpu_core_snapshot(&registry, cpu_index, snapshot);
    gate_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum percpu_status percpu_runtime_validate(void)
{
    bool interrupts_were_enabled;
    enum percpu_status status;

    if (!percpu_runtime_ready()) {
        return unavailable_status();
    }
    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    gate_lock();
    status = percpu_core_validate(&registry);
    if (status == PERCPU_STATUS_OK) {
        for (uint32_t index = 0U; index < PERCPU_CPU_LIMIT; ++index) {
            const bool published_online =
                atomic_load_acquire(&published[index].online) == UINT32_C(1);
            const struct percpu_core_cpu *cpu = &registry.cpus[index];

            if (published_online != (cpu->online == UINT8_C(1)) ||
                (published_online &&
                    (atomic_load_acquire(&published[index].apic_id) !=
                        cpu->apic_id ||
                     atomic_load_acquire(&published[index].generation) !=
                        cpu->generation))) {
                registry.poisoned = UINT8_C(1);
                atomic_store_release(&runtime_poisoned, UINT32_C(1));
                status = PERCPU_STATUS_CORRUPTED;
                break;
            }
        }
    }
    if (status == PERCPU_STATUS_POISONED ||
        status == PERCPU_STATUS_CORRUPTED) {
        atomic_store_release(&runtime_poisoned, UINT32_C(1));
    }
    gate_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

const struct percpu_core_state *percpu_runtime_core_state(void)
{
    return &registry;
}

bool percpu_runtime_registry_lock(void)
{
    if (!percpu_runtime_ready() || cpu_interrupts_enabled()) {
        return false;
    }
    gate_lock();
    if (registry.initialized != UINT8_C(1) ||
        registry.poisoned != 0U) {
        gate_unlock();
        return false;
    }
    return true;
}

void percpu_runtime_registry_unlock(void)
{
    gate_unlock();
}

const char *percpu_status_string(enum percpu_status status)
{
    switch (status) {
    case PERCPU_STATUS_OK:
        return "ok";
    case PERCPU_STATUS_NULL_ARGUMENT:
        return "null per-CPU argument";
    case PERCPU_STATUS_ALREADY_INITIALIZED:
        return "per-CPU registry initialized twice";
    case PERCPU_STATUS_NOT_INITIALIZED:
        return "per-CPU registry is not initialized";
    case PERCPU_STATUS_POISONED:
        return "per-CPU registry is poisoned";
    case PERCPU_STATUS_INVALID_CPU:
        return "invalid per-CPU index";
    case PERCPU_STATUS_INVALID_APIC_ID:
        return "invalid local APIC ID";
    case PERCPU_STATUS_CPU_ALREADY_ONLINE:
        return "per-CPU slot already online";
    case PERCPU_STATUS_CPU_OFFLINE:
        return "per-CPU slot is offline";
    case PERCPU_STATUS_DUPLICATE_APIC_ID:
        return "duplicate local APIC ID";
    case PERCPU_STATUS_BOOT_CPU:
        return "cannot offline the bootstrap CPU";
    case PERCPU_STATUS_GENERATION_EXHAUSTED:
        return "per-CPU generation exhausted";
    case PERCPU_STATUS_STALE_HANDLE:
        return "stale per-CPU handle";
    case PERCPU_STATUS_CORRUPTED:
        return "per-CPU registry is corrupted";
    default:
        return "unknown per-CPU status";
    }
}

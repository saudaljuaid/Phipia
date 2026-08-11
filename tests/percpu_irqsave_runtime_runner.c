/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/percpu.h>
#include <zenith/preempt.h>
#include <zenith/spinlock.h>

#define STRESS_ITERATIONS UINT32_C(250000)

static _Thread_local bool test_interrupts_enabled = true;
static _Thread_local uint32_t test_apic_id = 3U;
static volatile uint32_t reschedule_count;
static volatile uint32_t reschedule_observed_if_clear;
static uint64_t protected_counter;
static struct spinlock stress_lock;

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
    result->ebx = leaf == UINT32_C(1) ? test_apic_id << 24U : 0U;
    result->ecx = 0U;
    result->edx = 0U;
}

void interrupt_trigger_reschedule(void)
{
    if (!test_interrupts_enabled) {
        __atomic_store_n(
            &reschedule_observed_if_clear, UINT32_C(1), __ATOMIC_RELEASE
        );
    }
    (void)__atomic_add_fetch(&reschedule_count, 1U, __ATOMIC_ACQ_REL);
    (void)preempt_runtime_take_reschedule();
}

static bool initialize_runtime(uint32_t additional_cpu_count)
{
    struct percpu_handle handle;

    test_apic_id = 3U;
    test_interrupts_enabled = true;
    if (percpu_runtime_initialize(0U, 3U) != PERCPU_STATUS_OK ||
        !test_interrupts_enabled ||
        preempt_runtime_initialize() != PREEMPT_STATUS_OK ||
        !test_interrupts_enabled) {
        return false;
    }
    for (uint32_t cpu_index = 1U; cpu_index <= additional_cpu_count;
        ++cpu_index) {
        const uint32_t apic_id = 3U + cpu_index * 2U;

        if (percpu_runtime_register_cpu(cpu_index, apic_id, &handle) !=
                PERCPU_STATUS_OK ||
            handle.cpu_index != cpu_index || handle.generation == 0U ||
            preempt_runtime_cpu_online(cpu_index) != PREEMPT_STATUS_OK) {
            return false;
        }
    }
    return spinlock_runtime_initialize() == SPINLOCK_STATUS_OK &&
        test_interrupts_enabled;
}

static bool scenario_basic(void)
{
    static struct spinlock scheduler_lock;
    static struct spinlock heap_lock;
    struct spinlock_irq_token outer;
    struct spinlock_irq_token inner;
    struct spinlock_snapshot lock_snapshot;
    struct preempt_cpu_snapshot preempt_snapshot;
    struct percpu_snapshot cpu_snapshot;
    struct percpu_handle handle;
    bool switch_allowed = true;
    bool eligible = true;
    uint32_t cpu_index = 77U;

    if (percpu_runtime_current_cpu(&cpu_index) !=
            PERCPU_STATUS_NOT_INITIALIZED ||
        cpu_index != PERCPU_INVALID_CPU ||
        !initialize_runtime(1U) ||
        percpu_runtime_validate() != PERCPU_STATUS_OK ||
        spinlock_runtime_register(
            &scheduler_lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_runtime_register(
            &heap_lock, 1U, SPINLOCK_CLASS_HEAP, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_runtime_validate() != SPINLOCK_STATUS_OK ||
        percpu_runtime_current_cpu(&cpu_index) != PERCPU_STATUS_OK ||
        cpu_index != 0U ||
        percpu_runtime_current_handle(&handle) != PERCPU_STATUS_OK ||
        handle.cpu_index != 0U ||
        percpu_runtime_resolve(&handle, &cpu_index) != PERCPU_STATUS_OK ||
        cpu_index != 0U ||
        percpu_runtime_snapshot(1U, &cpu_snapshot) != PERCPU_STATUS_OK ||
        !cpu_snapshot.online || cpu_snapshot.boot_cpu ||
        cpu_snapshot.apic_id != 5U) {
        return false;
    }

    if (spinlock_irqsave_acquire(&scheduler_lock, &outer) !=
            SPINLOCK_STATUS_OK || test_interrupts_enabled ||
        preempt_runtime_snapshot(&preempt_snapshot) != PREEMPT_STATUS_OK ||
        preempt_snapshot.preempt_count != 1U ||
        preempt_runtime_switch_allowed(&switch_allowed) !=
            PREEMPT_STATUS_OK || switch_allowed ||
        spinlock_irqsave_acquire(&heap_lock, &inner) !=
            SPINLOCK_STATUS_OK || test_interrupts_enabled ||
        preempt_runtime_snapshot(&preempt_snapshot) != PREEMPT_STATUS_OK ||
        preempt_snapshot.preempt_count != 1U ||
        spinlock_irqrestore_release(&heap_lock, &inner) !=
            SPINLOCK_STATUS_OK || test_interrupts_enabled ||
        spinlock_irqrestore_release(&scheduler_lock, &outer) !=
            SPINLOCK_STATUS_OK || !test_interrupts_enabled ||
        preempt_runtime_snapshot(&preempt_snapshot) != PREEMPT_STATUS_OK ||
        preempt_snapshot.preempt_count != 0U ||
        preempt_runtime_switch_allowed(&switch_allowed) !=
            PREEMPT_STATUS_OK || !switch_allowed ||
        spinlock_runtime_snapshot(&scheduler_lock, &lock_snapshot) !=
            SPINLOCK_STATUS_OK || lock_snapshot.locked ||
        lock_snapshot.owner_cpu != SPINLOCK_INVALID_OWNER) {
        return false;
    }

    if (spinlock_irqsave_acquire(&scheduler_lock, &outer) !=
            SPINLOCK_STATUS_OK ||
        preempt_runtime_request_reschedule() != PREEMPT_STATUS_OK ||
        spinlock_irqrestore_release(&scheduler_lock, &outer) !=
            SPINLOCK_STATUS_OK || !test_interrupts_enabled ||
        __atomic_load_n(&reschedule_count, __ATOMIC_ACQUIRE) != 1U ||
        __atomic_load_n(
            &reschedule_observed_if_clear, __ATOMIC_ACQUIRE
        ) != 1U) {
        return false;
    }

    test_apic_id = 5U;
    if (percpu_runtime_current_cpu(&cpu_index) != PERCPU_STATUS_OK ||
        cpu_index != 1U ||
        preempt_runtime_irq_enter() != PREEMPT_STATUS_OK ||
        preempt_runtime_snapshot(&preempt_snapshot) != PREEMPT_STATUS_OK ||
        preempt_snapshot.irq_depth != 1U ||
        preempt_runtime_irq_exit(&eligible) != PREEMPT_STATUS_OK ||
        eligible) {
        return false;
    }
    return true;
}

static bool scenario_early_boot(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token early;
    struct spinlock_irq_token runtime;
    struct preempt_cpu_snapshot snapshot;

    test_apic_id = 3U;
    test_interrupts_enabled = true;
    if (percpu_runtime_initialize(0U, 3U) != PERCPU_STATUS_OK ||
        !test_interrupts_enabled ||
        spinlock_runtime_initialize() != SPINLOCK_STATUS_NOT_INITIALIZED ||
        !test_interrupts_enabled) {
        return false;
    }
    cpu_interrupt_disable();
    if (spinlock_runtime_initialize() != SPINLOCK_STATUS_OK ||
        test_interrupts_enabled ||
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_CPU_STATE, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_irqsave_acquire(&lock, &early) != SPINLOCK_STATUS_OK ||
        test_interrupts_enabled || early.preempt.runtime_present ||
        spinlock_irqrestore_release(&lock, &early) != SPINLOCK_STATUS_OK ||
        test_interrupts_enabled ||
        preempt_runtime_initialize() != PREEMPT_STATUS_OK ||
        spinlock_irqsave_acquire(&lock, &runtime) != SPINLOCK_STATUS_OK ||
        !runtime.preempt.runtime_present ||
        preempt_runtime_snapshot(&snapshot) != PREEMPT_STATUS_OK ||
        snapshot.preempt_count != 1U ||
        spinlock_irqrestore_release(&lock, &runtime) != SPINLOCK_STATUS_OK ||
        test_interrupts_enabled ||
        preempt_runtime_snapshot(&snapshot) != PREEMPT_STATUS_OK ||
        snapshot.preempt_count != 0U) {
        return false;
    }
    return true;
}

static bool scenario_recursive(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token first;
    struct spinlock_irq_token second;

    return initialize_runtime(0U) &&
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(&lock, &first) == SPINLOCK_STATUS_OK &&
        !test_interrupts_enabled &&
        spinlock_irqsave_acquire(&lock, &second) ==
            SPINLOCK_STATUS_RECURSIVE_LOCK &&
        !test_interrupts_enabled && !second.preempt.active &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_POISONED;
}

static bool scenario_order(void)
{
    static struct spinlock inner_class;
    static struct spinlock outer_class;
    struct spinlock_irq_token first;
    struct spinlock_irq_token second;

    return initialize_runtime(0U) &&
        spinlock_runtime_register(
            &inner_class, 0U, SPINLOCK_CLASS_PHYSICAL_MEMORY, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_runtime_register(
            &outer_class, 1U, SPINLOCK_CLASS_HEAP, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(&inner_class, &first) ==
            SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(&outer_class, &second) ==
            SPINLOCK_STATUS_ORDER_INVERSION &&
        !test_interrupts_enabled && !second.preempt.active &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_POISONED;
}

static bool scenario_subsystem_order(void)
{
    static struct spinlock heap_lock;
    static struct spinlock virtual_memory_lock;
    static struct spinlock physical_memory_lock;
    struct spinlock_irq_token heap_token;
    struct spinlock_irq_token virtual_memory_token;
    struct spinlock_irq_token physical_memory_token;

    return initialize_runtime(0U) &&
        spinlock_runtime_register(
            &heap_lock, 2U, SPINLOCK_CLASS_HEAP, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_runtime_register(
            &virtual_memory_lock, 3U, SPINLOCK_CLASS_VIRTUAL_MEMORY, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_runtime_register(
            &physical_memory_lock, 4U, SPINLOCK_CLASS_PHYSICAL_MEMORY, 0U
        ) == SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(&heap_lock, &heap_token) ==
            SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(
            &virtual_memory_lock, &virtual_memory_token
        ) == SPINLOCK_STATUS_OK &&
        spinlock_irqsave_acquire(
            &physical_memory_lock, &physical_memory_token
        ) == SPINLOCK_STATUS_OK &&
        !test_interrupts_enabled &&
        spinlock_irqrestore_release(
            &physical_memory_lock, &physical_memory_token
        ) == SPINLOCK_STATUS_OK &&
        !test_interrupts_enabled &&
        spinlock_irqrestore_release(
            &virtual_memory_lock, &virtual_memory_token
        ) == SPINLOCK_STATUS_OK &&
        !test_interrupts_enabled &&
        spinlock_irqrestore_release(&heap_lock, &heap_token) ==
            SPINLOCK_STATUS_OK &&
        test_interrupts_enabled &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_OK;
}

static bool scenario_stale(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token token;
    struct spinlock_irq_token stale;

    if (!initialize_runtime(0U) ||
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_irqsave_acquire(&lock, &token) != SPINLOCK_STATUS_OK) {
        return false;
    }
    stale = token;
    if (spinlock_irqrestore_release(&lock, &token) != SPINLOCK_STATUS_OK ||
        !test_interrupts_enabled || token.preempt.active) {
        return false;
    }
    cpu_interrupt_disable();
    return spinlock_irqrestore_release(&lock, &stale) ==
            SPINLOCK_STATUS_CORRUPTED &&
        !test_interrupts_enabled && stale.preempt.active;
}

static bool scenario_tampered_token(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token token;

    if (!initialize_runtime(0U) ||
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_irqsave_acquire(&lock, &token) != SPINLOCK_STATUS_OK) {
        return false;
    }
    ++token.generation;
    return spinlock_irqrestore_release(&lock, &token) ==
            SPINLOCK_STATUS_STALE_TOKEN &&
        !test_interrupts_enabled && token.preempt.active &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_POISONED;
}

static bool scenario_wrong_owner(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token token;

    if (!initialize_runtime(1U) ||
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK ||
        spinlock_irqsave_acquire(&lock, &token) != SPINLOCK_STATUS_OK) {
        return false;
    }
    test_apic_id = 5U;
    return spinlock_irqrestore_release(&lock, &token) ==
            SPINLOCK_STATUS_WRONG_OWNER &&
        !test_interrupts_enabled && token.preempt.active &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_POISONED;
}

static bool scenario_corruption(void)
{
    static struct spinlock lock;
    struct spinlock_irq_token token;

    if (!initialize_runtime(0U) ||
        spinlock_runtime_register(
            &lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    __atomic_store_n(&lock.word, UINT32_C(2), __ATOMIC_RELEASE);
    return spinlock_irqsave_acquire(&lock, &token) ==
            SPINLOCK_STATUS_CORRUPTED &&
        !test_interrupts_enabled && token.preempt.active &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_POISONED;
}

static bool scenario_overflow(void)
{
    bool eligible = true;

    if (!initialize_runtime(0U)) {
        return false;
    }
    for (uint32_t depth = 0U; depth < PREEMPT_NESTING_LIMIT; ++depth) {
        if (preempt_runtime_irq_enter() != PREEMPT_STATUS_OK) {
            return false;
        }
    }
    return preempt_runtime_irq_enter() == PREEMPT_STATUS_COUNTER_OVERFLOW &&
        preempt_runtime_irq_exit(&eligible) == PREEMPT_STATUS_POISONED &&
        !eligible;
}

struct stress_context {
    uint32_t apic_id;
};

static void *stress_thread(void *opaque)
{
    const struct stress_context *context =
        (const struct stress_context *)opaque;

    test_apic_id = context->apic_id;
    test_interrupts_enabled = true;
    for (uint32_t iteration = 0U; iteration < STRESS_ITERATIONS;
        ++iteration) {
        struct spinlock_irq_token token;

        if (spinlock_irqsave_acquire(&stress_lock, &token) !=
                SPINLOCK_STATUS_OK) {
            return (void *)(uintptr_t)1U;
        }
        ++protected_counter;
        if (spinlock_irqrestore_release(&stress_lock, &token) !=
                SPINLOCK_STATUS_OK || !test_interrupts_enabled) {
            return (void *)(uintptr_t)2U;
        }
    }
    return NULL;
}

static bool scenario_contention(void)
{
    const struct stress_context contexts[4] = {
        {.apic_id = 3U},
        {.apic_id = 5U},
        {.apic_id = 7U},
        {.apic_id = 9U}
    };
    pthread_t threads[4];
    void *results[4];

    if (!initialize_runtime(3U) ||
        spinlock_runtime_register(
            &stress_lock, 0U, SPINLOCK_CLASS_SCHEDULER, 0U
        ) != SPINLOCK_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        if (pthread_create(
                &threads[index], NULL, stress_thread,
                (void *)&contexts[index]
            ) != 0) {
            return false;
        }
    }
    for (size_t index = 0U; index < 4U; ++index) {
        if (pthread_join(threads[index], &results[index]) != 0 ||
            results[index] != NULL) {
            return false;
        }
    }
    return protected_counter == UINT64_C(1000000) &&
        spinlock_runtime_validate() == SPINLOCK_STATUS_OK;
}

int main(int argc, char **argv)
{
    bool passed = false;

    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "basic") == 0) {
        passed = scenario_basic();
    } else if (strcmp(argv[1], "early-boot") == 0) {
        passed = scenario_early_boot();
    } else if (strcmp(argv[1], "recursive") == 0) {
        passed = scenario_recursive();
    } else if (strcmp(argv[1], "order") == 0) {
        passed = scenario_order();
    } else if (strcmp(argv[1], "subsystem-order") == 0) {
        passed = scenario_subsystem_order();
    } else if (strcmp(argv[1], "stale") == 0) {
        passed = scenario_stale();
    } else if (strcmp(argv[1], "tampered") == 0) {
        passed = scenario_tampered_token();
    } else if (strcmp(argv[1], "wrong-owner") == 0) {
        passed = scenario_wrong_owner();
    } else if (strcmp(argv[1], "corruption") == 0) {
        passed = scenario_corruption();
    } else if (strcmp(argv[1], "overflow") == 0) {
        passed = scenario_overflow();
    } else if (strcmp(argv[1], "contention") == 0) {
        passed = scenario_contention();
    }
    if (!passed) {
        return 3;
    }
    printf("per-CPU irqsave runtime %s passed\n", argv[1]);
    return 0;
}

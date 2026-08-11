/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/cpu.h>
#include <zenith/percpu.h>
#include <zenith/spinlock.h>

#include "percpu_core.h"
#include "preempt_core.h"
#include "spinlock_core.h"

static struct spinlock_core_state runtime_state;
static struct spinlock *runtime_locks[SPINLOCK_LOCK_LIMIT];
static volatile uint32_t metadata_gate;
static volatile uint32_t runtime_initialized;

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
                &metadata_gate, &expected, UINT32_C(1), false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        cpu_relax();
    }
}

static void gate_unlock(void)
{
    atomic_store_release(&metadata_gate, 0U);
}

static void restore_interrupt_state(bool enabled)
{
    if (enabled) {
        cpu_interrupt_enable();
    } else {
        cpu_interrupt_disable();
    }
}

static void token_clear(struct spinlock_irq_token *token)
{
    token->cpu_index = PERCPU_INVALID_CPU;
    token->lock_index = SPINLOCK_LOCK_LIMIT;
    token->generation = 0U;
    token->interrupts_were_enabled = false;
    token->preempt.cpu.cpu_index = PERCPU_INVALID_CPU;
    token->preempt.cpu.generation = 0U;
    token->preempt.restore_interrupts = false;
    token->preempt.active = false;
    token->preempt.accounted = false;
    token->preempt.runtime_present = false;
}

static enum spinlock_status status_from_preempt(enum preempt_status status)
{
    if (status == PREEMPT_STATUS_NOT_INITIALIZED) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }
    if (status == PREEMPT_STATUS_CPU_OFFLINE) {
        return SPINLOCK_STATUS_CPU_OFFLINE;
    }
    if (status == PREEMPT_STATUS_INVALID_CPU) {
        return SPINLOCK_STATUS_INVALID_CPU;
    }
    return SPINLOCK_STATUS_CORRUPTED;
}

static enum spinlock_status poison(enum spinlock_status status)
{
    runtime_state.poisoned = UINT8_C(1);
    return status;
}

bool spinlock_runtime_ready(void)
{
    return atomic_load_acquire(&runtime_initialized) == UINT32_C(1);
}

enum spinlock_status spinlock_runtime_initialize(void)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    bool switch_allowed = false;
    uint32_t early_cpu = PERCPU_INVALID_CPU;
    enum preempt_status preempt_status;
    enum spinlock_status status;

    cpu_interrupt_disable();
    preempt_status = preempt_runtime_switch_allowed(&switch_allowed);
    if (preempt_status == PREEMPT_STATUS_NOT_INITIALIZED &&
        !interrupts_were_enabled &&
        percpu_runtime_current_cpu(&early_cpu) == PERCPU_STATUS_OK) {
        switch_allowed = true;
    } else if (preempt_status != PREEMPT_STATUS_OK) {
        restore_interrupt_state(interrupts_were_enabled);
        return status_from_preempt(preempt_status);
    } else if (!switch_allowed) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_CPU_BUSY;
    }
    if (!percpu_runtime_registry_lock()) {
        const enum percpu_status cpu_status = percpu_runtime_validate();

        restore_interrupt_state(interrupts_were_enabled);
        return cpu_status == PERCPU_STATUS_NOT_INITIALIZED ?
            SPINLOCK_STATUS_NOT_INITIALIZED : SPINLOCK_STATUS_CORRUPTED;
    }
    gate_lock();
    if (spinlock_runtime_ready()) {
        status = SPINLOCK_STATUS_ALREADY_INITIALIZED;
    } else {
        spinlock_core_clear(&runtime_state);
        for (size_t index = 0U; index < SPINLOCK_LOCK_LIMIT; ++index) {
            runtime_locks[index] = NULL;
        }
        status = spinlock_core_initialize(
            &runtime_state, percpu_runtime_core_state()
        );
        if (status == SPINLOCK_STATUS_OK) {
            atomic_store_release(&runtime_initialized, UINT32_C(1));
        }
    }
    gate_unlock();
    percpu_runtime_registry_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum spinlock_status spinlock_runtime_register(
    struct spinlock *lock,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
)
{
    bool interrupts_were_enabled;
    enum spinlock_status status;

    if (lock == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (!spinlock_runtime_ready()) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }

    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    if (!percpu_runtime_registry_lock()) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_CORRUPTED;
    }
    gate_lock();
    if (lock->registered != 0U ||
        atomic_load_acquire(&lock->word) != 0U ||
        lock->reserved[0] != 0U || lock->reserved[1] != 0U ||
        lock->reserved[2] != 0U) {
        status = poison(SPINLOCK_STATUS_CORRUPTED);
    } else {
        status = spinlock_core_register(
            &runtime_state, percpu_runtime_core_state(), lock_index,
            order_class, order_rank
        );
        if (status == SPINLOCK_STATUS_OK) {
            runtime_locks[lock_index] = lock;
            lock->lock_index = lock_index;
            lock->registered = UINT8_C(1);
        }
    }
    gate_unlock();
    percpu_runtime_registry_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

static bool lock_object_matches(const struct spinlock *lock)
{
    return lock->registered == UINT8_C(1) &&
        lock->lock_index < SPINLOCK_LOCK_LIMIT &&
        runtime_locks[lock->lock_index] == lock &&
        lock->reserved[0] == 0U && lock->reserved[1] == 0U &&
        lock->reserved[2] == 0U;
}

enum spinlock_status spinlock_irqsave_acquire(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    bool interrupts_were_enabled;
    bool fail_stop = false;
    uint32_t cpu_index;
    enum percpu_status cpu_status;
    enum preempt_status preempt_status;
    enum spinlock_status status;

    if (token == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    token_clear(token);
    if (lock == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (!spinlock_runtime_ready()) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }

    preempt_status = preempt_guard_enter(&token->preempt);
    if (preempt_status != PREEMPT_STATUS_OK) {
        status = status_from_preempt(preempt_status);
        token_clear(token);
        return status;
    }
    interrupts_were_enabled = token->preempt.restore_interrupts;
    cpu_status = percpu_runtime_current_cpu(&cpu_index);
    if (cpu_status != PERCPU_STATUS_OK) {
        status = cpu_status == PERCPU_STATUS_CPU_OFFLINE ?
            SPINLOCK_STATUS_CPU_OFFLINE : SPINLOCK_STATUS_CORRUPTED;
        (void)preempt_guard_exit(&token->preempt);
        token_clear(token);
        return status;
    }

    for (;;) {
        uint32_t expected;

        if (!percpu_runtime_registry_lock()) {
            status = SPINLOCK_STATUS_CORRUPTED;
            fail_stop = true;
            break;
        }
        gate_lock();
        if (!lock_object_matches(lock)) {
            status = poison(SPINLOCK_STATUS_INVALID_LOCK);
            fail_stop = true;
        } else if (atomic_load_acquire(&lock->word) > UINT32_C(1)) {
            status = poison(SPINLOCK_STATUS_CORRUPTED);
            fail_stop = true;
        } else {
            status = spinlock_core_irqsave_acquire(
                &runtime_state, percpu_runtime_core_state(),
                lock->lock_index, cpu_index, interrupts_were_enabled, token
            );
        }

        if (status == SPINLOCK_STATUS_OK) {
            expected = 0U;
            if (!__atomic_compare_exchange_n(
                    &lock->word, &expected, UINT32_C(1), false,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                status = poison(SPINLOCK_STATUS_CORRUPTED);
                fail_stop = true;
            }
        }
        gate_unlock();
        percpu_runtime_registry_unlock();

        if (status != SPINLOCK_STATUS_LOCK_BUSY) {
            break;
        }
        while (atomic_load_acquire(&lock->word) != 0U) {
            cpu_relax();
        }
    }

    if (status != SPINLOCK_STATUS_OK) {
        if (fail_stop || status == SPINLOCK_STATUS_POISONED ||
            status == SPINLOCK_STATUS_CORRUPTED) {
            preempt_runtime_fail_stop();
        } else {
            (void)preempt_guard_exit(&token->preempt);
            token_clear(token);
        }
    }
    return status;
}

enum spinlock_status spinlock_irqrestore_release(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    bool restore_interrupts = false;
    uint32_t cpu_index;
    enum percpu_status cpu_status;
    enum spinlock_status status;
    enum preempt_status preempt_status;

    cpu_interrupt_disable();
    if (lock == NULL || token == NULL) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (!spinlock_runtime_ready()) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }
    cpu_status = percpu_runtime_current_cpu(&cpu_index);
    if (cpu_status != PERCPU_STATUS_OK) {
        preempt_runtime_fail_stop();
        return cpu_status == PERCPU_STATUS_CPU_OFFLINE ?
            SPINLOCK_STATUS_CPU_OFFLINE : SPINLOCK_STATUS_CORRUPTED;
    }
    if (!percpu_runtime_registry_lock()) {
        preempt_runtime_fail_stop();
        return SPINLOCK_STATUS_CORRUPTED;
    }

    gate_lock();
    if (!token->preempt.active || token->cpu_index != cpu_index ||
        (token->preempt.runtime_present &&
            token->preempt.cpu.cpu_index != cpu_index)) {
        status = poison(SPINLOCK_STATUS_WRONG_OWNER);
    } else if (token->interrupts_were_enabled !=
        token->preempt.restore_interrupts) {
        status = poison(SPINLOCK_STATUS_STALE_TOKEN);
    } else if (!lock_object_matches(lock) ||
        token->lock_index != lock->lock_index) {
        status = poison(SPINLOCK_STATUS_INVALID_LOCK);
    } else if (atomic_load_acquire(&lock->word) != UINT32_C(1)) {
        status = poison(SPINLOCK_STATUS_CORRUPTED);
    } else {
        status = spinlock_core_irqrestore_release(
            &runtime_state, percpu_runtime_core_state(), token,
            interrupts_were_enabled, &restore_interrupts
        );
        if (status == SPINLOCK_STATUS_OK) {
            atomic_store_release(&lock->word, 0U);
        }
    }
    gate_unlock();
    percpu_runtime_registry_unlock();

    if (status == SPINLOCK_STATUS_OK) {
        if (restore_interrupts != token->preempt.restore_interrupts ||
            !token->preempt.active) {
            preempt_runtime_fail_stop();
            return SPINLOCK_STATUS_CORRUPTED;
        }
        preempt_status = preempt_guard_exit(&token->preempt);
        if (preempt_status != PREEMPT_STATUS_OK) {
            preempt_runtime_fail_stop();
            return SPINLOCK_STATUS_CORRUPTED;
        }
        token_clear(token);
    } else {
        /* Ownership is uncertain: keep IF clear and preemption pinned. */
        preempt_runtime_fail_stop();
    }
    return status;
}

enum spinlock_status spinlock_runtime_snapshot(
    const struct spinlock *lock,
    struct spinlock_snapshot *snapshot
)
{
    bool interrupts_were_enabled;
    enum spinlock_status status;

    if (snapshot == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    snapshot->owner_cpu = SPINLOCK_INVALID_OWNER;
    snapshot->generation = 0U;
    snapshot->order_class = SPINLOCK_CLASS_LIMIT;
    snapshot->order_rank = 0U;
    snapshot->registered = false;
    snapshot->locked = false;
    if (lock == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (!spinlock_runtime_ready()) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }

    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    if (!percpu_runtime_registry_lock()) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_CORRUPTED;
    }
    gate_lock();
    if (!lock_object_matches(lock)) {
        status = SPINLOCK_STATUS_INVALID_LOCK;
    } else {
        status = spinlock_core_snapshot(
            &runtime_state, percpu_runtime_core_state(), lock->lock_index,
            snapshot
        );
        if (status == SPINLOCK_STATUS_OK &&
            snapshot->locked !=
                (atomic_load_acquire(&lock->word) == UINT32_C(1))) {
            status = poison(SPINLOCK_STATUS_CORRUPTED);
        }
    }
    gate_unlock();
    percpu_runtime_registry_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

enum spinlock_status spinlock_runtime_validate(void)
{
    bool interrupts_were_enabled;
    enum spinlock_status status;

    if (!spinlock_runtime_ready()) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }
    interrupts_were_enabled = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    if (!percpu_runtime_registry_lock()) {
        restore_interrupt_state(interrupts_were_enabled);
        return SPINLOCK_STATUS_CORRUPTED;
    }
    gate_lock();
    status = spinlock_core_validate(
        &runtime_state, percpu_runtime_core_state()
    );
    if (status == SPINLOCK_STATUS_OK) {
        for (uint32_t index = 0U; index < SPINLOCK_LOCK_LIMIT; ++index) {
            const struct spinlock_core_lock *core =
                &runtime_state.locks[index];
            struct spinlock *lock = runtime_locks[index];

            if ((core->registered == UINT8_C(1)) != (lock != NULL) ||
                (lock != NULL &&
                    (!lock_object_matches(lock) ||
                     atomic_load_acquire(&lock->word) > UINT32_C(1) ||
                     (atomic_load_acquire(&lock->word) == UINT32_C(1)) !=
                        (core->locked == UINT8_C(1))))) {
                status = poison(SPINLOCK_STATUS_CORRUPTED);
                break;
            }
        }
    }
    gate_unlock();
    percpu_runtime_registry_unlock();
    restore_interrupt_state(interrupts_were_enabled);
    return status;
}

const char *spinlock_status_string(enum spinlock_status status)
{
    switch (status) {
    case SPINLOCK_STATUS_OK:
        return "ok";
    case SPINLOCK_STATUS_NULL_ARGUMENT:
        return "null spinlock argument";
    case SPINLOCK_STATUS_ALREADY_INITIALIZED:
        return "spinlock runtime initialized twice";
    case SPINLOCK_STATUS_NOT_INITIALIZED:
        return "spinlock runtime is not initialized";
    case SPINLOCK_STATUS_POISONED:
        return "spinlock runtime is poisoned";
    case SPINLOCK_STATUS_INVALID_CPU:
        return "invalid spinlock CPU";
    case SPINLOCK_STATUS_CPU_OFFLINE:
        return "spinlock CPU is offline";
    case SPINLOCK_STATUS_INVALID_LOCK:
        return "invalid spinlock";
    case SPINLOCK_STATUS_INVALID_ORDER:
        return "invalid spinlock order";
    case SPINLOCK_STATUS_LOCK_ALREADY_REGISTERED:
        return "spinlock registered twice";
    case SPINLOCK_STATUS_LOCK_NOT_REGISTERED:
        return "spinlock is not registered";
    case SPINLOCK_STATUS_LOCK_BUSY:
        return "spinlock is busy";
    case SPINLOCK_STATUS_CPU_BUSY:
        return "spinlock CPU still owns locks";
    case SPINLOCK_STATUS_RECURSIVE_LOCK:
        return "recursive spinlock acquisition";
    case SPINLOCK_STATUS_WRONG_OWNER:
        return "spinlock released by wrong owner";
    case SPINLOCK_STATUS_ORDER_INVERSION:
        return "spinlock order inversion";
    case SPINLOCK_STATUS_NESTING_OVERFLOW:
        return "spinlock nesting overflow";
    case SPINLOCK_STATUS_INTERRUPTS_ENABLED:
        return "spinlock release with interrupts enabled";
    case SPINLOCK_STATUS_STALE_TOKEN:
        return "stale spinlock token";
    case SPINLOCK_STATUS_GENERATION_EXHAUSTED:
        return "spinlock generation exhausted";
    case SPINLOCK_STATUS_CORRUPTED:
        return "spinlock runtime is corrupted";
    default:
        return "unknown spinlock status";
    }
}

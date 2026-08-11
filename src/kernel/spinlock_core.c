/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spinlock_core.h"

_Static_assert(PERCPU_CPU_LIMIT <= UINT8_MAX,
    "spinlock owner storage cannot represent every CPU");
_Static_assert(
    SPINLOCK_CLASS_CPU_STATE < SPINLOCK_CLASS_INTERRUPT &&
    SPINLOCK_CLASS_INTERRUPT < SPINLOCK_CLASS_TIME &&
    SPINLOCK_CLASS_TIME < SPINLOCK_CLASS_SCHEDULER &&
    SPINLOCK_CLASS_SCHEDULER < SPINLOCK_CLASS_DEVICE &&
    SPINLOCK_CLASS_DEVICE < SPINLOCK_CLASS_HEAP &&
    SPINLOCK_CLASS_HEAP < SPINLOCK_CLASS_VIRTUAL_MEMORY &&
    SPINLOCK_CLASS_VIRTUAL_MEMORY < SPINLOCK_CLASS_PHYSICAL_MEMORY,
    "spinlock classes no longer follow the kernel call graph"
);
_Static_assert(SPINLOCK_LOCK_LIMIT < UINT8_MAX,
    "spinlock stack storage cannot represent every lock");
_Static_assert(sizeof(struct spinlock_core_lock) == 16U,
    "spinlock debug record size changed");
_Static_assert(sizeof(struct spinlock_core_cpu) == 20U,
    "spinlock per-CPU debug state size changed");
_Static_assert(sizeof(struct spinlock_core_state) == 848U,
    "spinlock core state size changed");

#define INVALID_OWNER UINT8_MAX
#define INVALID_LOCK UINT8_MAX

static bool order_class_valid(uint8_t order_class)
{
    return order_class >= (uint8_t)SPINLOCK_CLASS_CPU_STATE &&
        order_class < (uint8_t)SPINLOCK_CLASS_LIMIT;
}

/* The caller has already run percpu_core_validate().  Keep the bounds check
 * local, but do not turn each online lookup into another full registry scan. */
static bool validated_cpu_is_online(
    const struct percpu_core_state *cpus,
    uint32_t cpu_index
)
{
    return cpu_index < PERCPU_CPU_LIMIT &&
        cpus->cpus[cpu_index].online == UINT8_C(1);
}

static bool order_before(
    const struct spinlock_core_lock *outer,
    const struct spinlock_core_lock *inner
)
{
    return outer->order_class < inner->order_class ||
        (outer->order_class == inner->order_class &&
            outer->order_rank < inner->order_rank);
}

void spinlock_core_clear(struct spinlock_core_state *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t index = 0U; index < SPINLOCK_LOCK_LIMIT; ++index) {
        state->locks[index].generation = 0U;
        state->locks[index].owner_cpu = INVALID_OWNER;
        state->locks[index].order_class = 0U;
        state->locks[index].order_rank = 0U;
        state->locks[index].registered = 0U;
        state->locks[index].locked = 0U;
        state->locks[index].reserved[0] = 0U;
        state->locks[index].reserved[1] = 0U;
        state->locks[index].reserved[2] = 0U;
    }
    for (size_t cpu = 0U; cpu < PERCPU_CPU_LIMIT; ++cpu) {
        for (size_t depth = 0U; depth < SPINLOCK_HELD_LIMIT; ++depth) {
            state->cpus[cpu].held_locks[depth] = INVALID_LOCK;
            state->cpus[cpu].saved_interrupt_state[depth] = 0U;
        }
        state->cpus[cpu].held_depth = 0U;
        state->cpus[cpu].reserved[0] = 0U;
        state->cpus[cpu].reserved[1] = 0U;
        state->cpus[cpu].reserved[2] = 0U;
    }
    state->next_generation = 1U;
    state->registered_count = 0U;
    state->initialized = 0U;
    state->poisoned = 0U;
    state->reserved[0] = 0U;
    state->reserved[1] = 0U;
}

static enum spinlock_status validate_cpu_stack(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    size_t cpu_index,
    uint32_t *observed_locked
)
{
    const struct spinlock_core_cpu *cpu = &state->cpus[cpu_index];

    if (cpu->held_depth > SPINLOCK_HELD_LIMIT || cpu->reserved[0] != 0U ||
        cpu->reserved[1] != 0U || cpu->reserved[2] != 0U) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    if (!validated_cpu_is_online(cpus, (uint32_t)cpu_index) &&
        cpu->held_depth != 0U) {
        return SPINLOCK_STATUS_CORRUPTED;
    }

    for (size_t depth = 0U; depth < SPINLOCK_HELD_LIMIT; ++depth) {
        const uint8_t lock_index = cpu->held_locks[depth];

        if (depth >= cpu->held_depth) {
            if (lock_index != INVALID_LOCK ||
                cpu->saved_interrupt_state[depth] != 0U) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
            continue;
        }
        if (lock_index >= SPINLOCK_LOCK_LIMIT ||
            cpu->saved_interrupt_state[depth] > UINT8_C(1)) {
            return SPINLOCK_STATUS_CORRUPTED;
        }
        if (depth != 0U &&
            cpu->saved_interrupt_state[depth] != 0U) {
            return SPINLOCK_STATUS_CORRUPTED;
        }

        const struct spinlock_core_lock *lock = &state->locks[lock_index];
        if (lock->registered != UINT8_C(1) || lock->locked != UINT8_C(1) ||
            lock->owner_cpu != cpu_index) {
            return SPINLOCK_STATUS_CORRUPTED;
        }
        if (depth != 0U) {
            const uint8_t outer_index = cpu->held_locks[depth - 1U];
            if (!order_before(&state->locks[outer_index], lock)) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
        }
        for (size_t earlier = 0U; earlier < depth; ++earlier) {
            if (cpu->held_locks[earlier] == lock_index) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
        }
        ++*observed_locked;
    }
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_core_validate(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus
)
{
    uint32_t observed_registered = 0U;
    uint32_t observed_locked = 0U;

    if (state == NULL || cpus == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (percpu_core_validate(cpus) != PERCPU_STATUS_OK) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    if (state->poisoned > UINT8_C(1)) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    if (state->poisoned == UINT8_C(1)) {
        return SPINLOCK_STATUS_POISONED;
    }
    if (state->initialized == 0U) {
        return SPINLOCK_STATUS_NOT_INITIALIZED;
    }
    if (state->initialized != UINT8_C(1) || state->next_generation == 0U ||
        state->reserved[0] != 0U || state->reserved[1] != 0U) {
        return SPINLOCK_STATUS_CORRUPTED;
    }

    for (size_t index = 0U; index < SPINLOCK_LOCK_LIMIT; ++index) {
        const struct spinlock_core_lock *lock = &state->locks[index];
        if (lock->registered > UINT8_C(1) || lock->locked > UINT8_C(1) ||
            lock->reserved[0] != 0U || lock->reserved[1] != 0U ||
            lock->reserved[2] != 0U || lock->generation >= state->next_generation) {
            return SPINLOCK_STATUS_CORRUPTED;
        }
        if (lock->registered == 0U) {
            if (lock->generation != 0U || lock->owner_cpu != INVALID_OWNER ||
                lock->order_class != 0U || lock->order_rank != 0U ||
                lock->locked != 0U) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
            continue;
        }
        ++observed_registered;
        if (!order_class_valid(lock->order_class)) {
            return SPINLOCK_STATUS_CORRUPTED;
        }
        if (lock->locked == 0U) {
            if (lock->owner_cpu != INVALID_OWNER) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
        } else if (lock->generation == 0U ||
            lock->owner_cpu >= PERCPU_CPU_LIMIT ||
            !validated_cpu_is_online(cpus, lock->owner_cpu)) {
            return SPINLOCK_STATUS_CORRUPTED;
        }
    }

    for (size_t cpu = 0U; cpu < PERCPU_CPU_LIMIT; ++cpu) {
        enum spinlock_status status = validate_cpu_stack(
            state, cpus, cpu, &observed_locked
        );
        if (status != SPINLOCK_STATUS_OK) {
            return status;
        }
    }
    if (observed_registered != state->registered_count ||
        observed_registered > SPINLOCK_LOCK_LIMIT) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    for (size_t index = 0U; index < SPINLOCK_LOCK_LIMIT; ++index) {
        if (state->locks[index].locked == UINT8_C(1)) {
            if (observed_locked == 0U) {
                return SPINLOCK_STATUS_CORRUPTED;
            }
            --observed_locked;
        }
    }
    if (observed_locked != 0U) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    return SPINLOCK_STATUS_OK;
}

static enum spinlock_status mutation_gate(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus
)
{
    enum spinlock_status status;

    if (state == NULL || cpus == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    status = spinlock_core_validate(state, cpus);
    if (status == SPINLOCK_STATUS_CORRUPTED) {
        state->poisoned = UINT8_C(1);
    }
    return status;
}

static enum spinlock_status poison(
    struct spinlock_core_state *state,
    enum spinlock_status status
)
{
    state->poisoned = UINT8_C(1);
    return status;
}

enum spinlock_status spinlock_core_initialize(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus
)
{
    if (state == NULL || cpus == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    if (state->initialized != 0U) {
        return SPINLOCK_STATUS_ALREADY_INITIALIZED;
    }
    if (percpu_core_validate(cpus) != PERCPU_STATUS_OK) {
        return SPINLOCK_STATUS_CORRUPTED;
    }
    spinlock_core_clear(state);
    state->initialized = UINT8_C(1);
    return spinlock_core_validate(state, cpus);
}

enum spinlock_status spinlock_core_register(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
)
{
    enum spinlock_status status = mutation_gate(state, cpus);

    if (status != SPINLOCK_STATUS_OK) {
        return status;
    }
    if (lock_index >= SPINLOCK_LOCK_LIMIT) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }
    if (!order_class_valid((uint8_t)order_class)) {
        return SPINLOCK_STATUS_INVALID_ORDER;
    }
    if (state->locks[lock_index].registered == UINT8_C(1)) {
        return SPINLOCK_STATUS_LOCK_ALREADY_REGISTERED;
    }
    state->locks[lock_index].order_class = (uint8_t)order_class;
    state->locks[lock_index].order_rank = order_rank;
    state->locks[lock_index].registered = UINT8_C(1);
    ++state->registered_count;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_core_irqsave_acquire(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    uint32_t cpu_index,
    bool interrupts_enabled,
    struct spinlock_irq_token *token
)
{
    enum spinlock_status status;
    struct spinlock_core_lock *lock;
    struct spinlock_core_cpu *cpu;
    size_t depth;

    if (token == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    token->cpu_index = PERCPU_INVALID_CPU;
    token->lock_index = SPINLOCK_LOCK_LIMIT;
    token->generation = 0U;
    token->interrupts_were_enabled = false;
    status = mutation_gate(state, cpus);
    if (status != SPINLOCK_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return SPINLOCK_STATUS_INVALID_CPU;
    }
    if (!validated_cpu_is_online(cpus, cpu_index)) {
        return SPINLOCK_STATUS_CPU_OFFLINE;
    }
    if (lock_index >= SPINLOCK_LOCK_LIMIT) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }
    lock = &state->locks[lock_index];
    cpu = &state->cpus[cpu_index];
    if (lock->registered == 0U) {
        return SPINLOCK_STATUS_LOCK_NOT_REGISTERED;
    }
    if (lock->locked == UINT8_C(1)) {
        if (lock->owner_cpu == cpu_index) {
            return poison(state, SPINLOCK_STATUS_RECURSIVE_LOCK);
        }
        return SPINLOCK_STATUS_LOCK_BUSY;
    }
    if (cpu->held_depth >= SPINLOCK_HELD_LIMIT) {
        return poison(state, SPINLOCK_STATUS_NESTING_OVERFLOW);
    }
    if (cpu->held_depth != 0U && interrupts_enabled) {
        return poison(state, SPINLOCK_STATUS_INTERRUPTS_ENABLED);
    }
    depth = cpu->held_depth;
    if (depth != 0U) {
        const uint8_t outer_index = cpu->held_locks[depth - 1U];
        if (!order_before(&state->locks[outer_index], lock)) {
            return poison(state, SPINLOCK_STATUS_ORDER_INVERSION);
        }
    }
    if (state->next_generation == UINT64_MAX) {
        return poison(state, SPINLOCK_STATUS_GENERATION_EXHAUSTED);
    }

    lock->generation = state->next_generation;
    ++state->next_generation;
    lock->owner_cpu = (uint8_t)cpu_index;
    lock->locked = UINT8_C(1);
    cpu->held_locks[depth] = (uint8_t)lock_index;
    cpu->saved_interrupt_state[depth] = interrupts_enabled ? UINT8_C(1) : 0U;
    ++cpu->held_depth;
    token->cpu_index = cpu_index;
    token->lock_index = lock_index;
    token->generation = lock->generation;
    token->interrupts_were_enabled = interrupts_enabled;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_core_irqrestore_release(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    const struct spinlock_irq_token *token,
    bool interrupts_enabled,
    bool *restore_interrupts
)
{
    enum spinlock_status status;
    struct spinlock_core_lock *lock;
    struct spinlock_core_cpu *cpu;
    size_t depth;

    if (token == NULL || restore_interrupts == NULL) {
        return SPINLOCK_STATUS_NULL_ARGUMENT;
    }
    *restore_interrupts = false;
    status = mutation_gate(state, cpus);
    if (status != SPINLOCK_STATUS_OK) {
        return status;
    }
    if (interrupts_enabled) {
        return poison(state, SPINLOCK_STATUS_INTERRUPTS_ENABLED);
    }
    if (token->cpu_index >= PERCPU_CPU_LIMIT) {
        return poison(state, SPINLOCK_STATUS_WRONG_OWNER);
    }
    if (!validated_cpu_is_online(cpus, token->cpu_index)) {
        return poison(state, SPINLOCK_STATUS_CPU_OFFLINE);
    }
    if (token->lock_index >= SPINLOCK_LOCK_LIMIT) {
        return poison(state, SPINLOCK_STATUS_INVALID_LOCK);
    }
    lock = &state->locks[token->lock_index];
    cpu = &state->cpus[token->cpu_index];
    if (lock->registered == 0U) {
        return poison(state, SPINLOCK_STATUS_LOCK_NOT_REGISTERED);
    }
    if (lock->locked == 0U || lock->generation != token->generation ||
        token->generation == 0U) {
        return poison(state, SPINLOCK_STATUS_STALE_TOKEN);
    }
    if (lock->owner_cpu != token->cpu_index || cpu->held_depth == 0U) {
        return poison(state, SPINLOCK_STATUS_WRONG_OWNER);
    }
    depth = (size_t)cpu->held_depth - 1U;
    if (cpu->held_locks[depth] != token->lock_index) {
        return poison(state, SPINLOCK_STATUS_ORDER_INVERSION);
    }
    if ((cpu->saved_interrupt_state[depth] != 0U) !=
        token->interrupts_were_enabled) {
        return poison(state, SPINLOCK_STATUS_STALE_TOKEN);
    }

    lock->owner_cpu = INVALID_OWNER;
    lock->locked = 0U;
    cpu->held_locks[depth] = INVALID_LOCK;
    cpu->saved_interrupt_state[depth] = 0U;
    --cpu->held_depth;
    *restore_interrupts = token->interrupts_were_enabled;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_core_cpu_can_offline(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t cpu_index
)
{
    enum spinlock_status status = spinlock_core_validate(state, cpus);

    if (status != SPINLOCK_STATUS_OK) {
        return status;
    }
    if (cpu_index >= PERCPU_CPU_LIMIT) {
        return SPINLOCK_STATUS_INVALID_CPU;
    }
    if (!validated_cpu_is_online(cpus, cpu_index)) {
        return SPINLOCK_STATUS_CPU_OFFLINE;
    }
    return state->cpus[cpu_index].held_depth == 0U ?
        SPINLOCK_STATUS_OK : SPINLOCK_STATUS_CPU_BUSY;
}

enum spinlock_status spinlock_core_snapshot(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    struct spinlock_snapshot *snapshot
)
{
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
    status = spinlock_core_validate(state, cpus);
    if (status != SPINLOCK_STATUS_OK) {
        return status;
    }
    if (lock_index >= SPINLOCK_LOCK_LIMIT) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }
    snapshot->owner_cpu = state->locks[lock_index].owner_cpu == INVALID_OWNER ?
        SPINLOCK_INVALID_OWNER : state->locks[lock_index].owner_cpu;
    snapshot->generation = state->locks[lock_index].generation;
    snapshot->order_class =
        (enum spinlock_order_class)state->locks[lock_index].order_class;
    snapshot->order_rank = state->locks[lock_index].order_rank;
    snapshot->registered = state->locks[lock_index].registered == UINT8_C(1);
    snapshot->locked = state->locks[lock_index].locked == UINT8_C(1);
    return SPINLOCK_STATUS_OK;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SPINLOCK_CORE_H
#define ZENITH_SPINLOCK_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/spinlock.h>

#include "percpu_core.h"

struct spinlock_core_lock {
    uint64_t generation;
    uint8_t owner_cpu;
    uint8_t order_class;
    uint8_t order_rank;
    uint8_t registered;
    uint8_t locked;
    uint8_t reserved[3];
};

struct spinlock_core_cpu {
    uint8_t held_locks[SPINLOCK_HELD_LIMIT];
    uint8_t saved_interrupt_state[SPINLOCK_HELD_LIMIT];
    uint8_t held_depth;
    uint8_t reserved[3];
};

struct spinlock_core_state {
    struct spinlock_core_lock locks[SPINLOCK_LOCK_LIMIT];
    struct spinlock_core_cpu cpus[PERCPU_CPU_LIMIT];
    uint64_t next_generation;
    uint32_t registered_count;
    uint8_t initialized;
    uint8_t poisoned;
    uint8_t reserved[2];
};

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void spinlock_core_clear(struct spinlock_core_state *state);
enum spinlock_status spinlock_core_initialize(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus
);
enum spinlock_status spinlock_core_validate(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus
);
enum spinlock_status spinlock_core_register(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
);
enum spinlock_status spinlock_core_irqsave_acquire(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    uint32_t cpu_index,
    bool interrupts_enabled,
    struct spinlock_irq_token *token
);
enum spinlock_status spinlock_core_irqrestore_release(
    struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    const struct spinlock_irq_token *token,
    bool interrupts_enabled,
    bool *restore_interrupts
);
enum spinlock_status spinlock_core_cpu_can_offline(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t cpu_index
);
enum spinlock_status spinlock_core_snapshot(
    const struct spinlock_core_state *state,
    const struct percpu_core_state *cpus,
    uint32_t lock_index,
    struct spinlock_snapshot *snapshot
);

#endif

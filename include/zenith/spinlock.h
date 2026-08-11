/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SPINLOCK_H
#define ZENITH_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/preempt.h>

#define SPINLOCK_LOCK_LIMIT UINT32_C(32)
#define SPINLOCK_HELD_LIMIT UINT32_C(8)
#define SPINLOCK_INVALID_OWNER UINT32_MAX

/*
 * Classes are a global outer-to-inner order.  The sequence encodes today's
 * call graph: timer interrupt work may enter the scheduler, and scheduler or
 * device work may allocate through heap -> virtual memory -> physical memory.
 * A subsystem must use an explicitly unlocked transaction instead of nesting
 * against this direction.  Locks in one class additionally use their rank,
 * also from low to high.  Equal keys may never nest.
 */
enum spinlock_order_class {
    SPINLOCK_CLASS_CPU_STATE = 1,
    SPINLOCK_CLASS_INTERRUPT,
    SPINLOCK_CLASS_TIME,
    SPINLOCK_CLASS_SCHEDULER,
    SPINLOCK_CLASS_DEVICE,
    SPINLOCK_CLASS_HEAP,
    SPINLOCK_CLASS_VIRTUAL_MEMORY,
    SPINLOCK_CLASS_PHYSICAL_MEMORY,
    SPINLOCK_CLASS_LIMIT
};

enum spinlock_status {
    SPINLOCK_STATUS_OK = 0,
    SPINLOCK_STATUS_NULL_ARGUMENT,
    SPINLOCK_STATUS_ALREADY_INITIALIZED,
    SPINLOCK_STATUS_NOT_INITIALIZED,
    SPINLOCK_STATUS_POISONED,
    SPINLOCK_STATUS_INVALID_CPU,
    SPINLOCK_STATUS_CPU_OFFLINE,
    SPINLOCK_STATUS_INVALID_LOCK,
    SPINLOCK_STATUS_INVALID_ORDER,
    SPINLOCK_STATUS_LOCK_ALREADY_REGISTERED,
    SPINLOCK_STATUS_LOCK_NOT_REGISTERED,
    SPINLOCK_STATUS_LOCK_BUSY,
    SPINLOCK_STATUS_CPU_BUSY,
    SPINLOCK_STATUS_RECURSIVE_LOCK,
    SPINLOCK_STATUS_WRONG_OWNER,
    SPINLOCK_STATUS_ORDER_INVERSION,
    SPINLOCK_STATUS_NESTING_OVERFLOW,
    SPINLOCK_STATUS_INTERRUPTS_ENABLED,
    SPINLOCK_STATUS_STALE_TOKEN,
    SPINLOCK_STATUS_GENERATION_EXHAUSTED,
    SPINLOCK_STATUS_CORRUPTED
};

struct spinlock_irq_token {
    uint32_t cpu_index;
    uint32_t lock_index;
    uint64_t generation;
    bool interrupts_were_enabled;
    struct preempt_guard preempt;
};

struct spinlock_snapshot {
    uint32_t owner_cpu;
    uint64_t generation;
    enum spinlock_order_class order_class;
    uint8_t order_rank;
    bool registered;
    bool locked;
};

/* Static storage is zero-initialized before spinlock_runtime_register(). */
struct spinlock {
    volatile uint32_t word;
    uint32_t lock_index;
    uint8_t registered;
    uint8_t reserved[3];
};

enum spinlock_status spinlock_runtime_initialize(void);
enum spinlock_status spinlock_runtime_register(
    struct spinlock *lock,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
);
enum spinlock_status spinlock_irqsave_acquire(
    struct spinlock *lock,
    struct spinlock_irq_token *token
);
enum spinlock_status spinlock_irqrestore_release(
    struct spinlock *lock,
    struct spinlock_irq_token *token
);
enum spinlock_status spinlock_runtime_snapshot(
    const struct spinlock *lock,
    struct spinlock_snapshot *snapshot
);
enum spinlock_status spinlock_runtime_validate(void);
bool spinlock_runtime_ready(void);
const char *spinlock_status_string(enum spinlock_status status);

#endif

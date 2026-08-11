/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_PREEMPT_CORE_H
#define ZENITH_PREEMPT_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/preempt.h>

struct preempt_core_cpu {
    uint32_t irq_depth;
    uint32_t preempt_count;
    uint8_t need_resched;
    uint8_t online;
    uint8_t reserved[2];
};

struct preempt_core_state {
    struct preempt_core_cpu cpus[PREEMPT_CPU_LIMIT];
    uint32_t online_count;
    uint8_t initialized;
    uint8_t poisoned;
    uint8_t reserved[2];
};

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void preempt_core_clear(struct preempt_core_state *state);
enum preempt_status preempt_core_initialize(
    struct preempt_core_state *state,
    uint32_t boot_cpu
);
enum preempt_status preempt_core_validate(
    const struct preempt_core_state *state
);
enum preempt_status preempt_core_cpu_online(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_cpu_offline(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_irq_enter(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_irq_exit(
    struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *reschedule_eligible
);
enum preempt_status preempt_core_disable(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_enable(
    struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *reschedule_eligible
);
enum preempt_status preempt_core_request_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_cancel_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_reschedule_eligible(
    const struct preempt_core_state *state,
    uint32_t cpu_index,
    bool *eligible
);
enum preempt_status preempt_core_take_reschedule(
    struct preempt_core_state *state,
    uint32_t cpu_index
);
enum preempt_status preempt_core_cpu_snapshot(
    const struct preempt_core_state *state,
    uint32_t cpu_index,
    struct preempt_cpu_snapshot *snapshot
);

/* Runtime-private terminal safety gate used after uncertain lock ownership. */
void preempt_runtime_fail_stop(void);

#endif

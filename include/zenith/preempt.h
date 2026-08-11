/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_PREEMPT_H
#define ZENITH_PREEMPT_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/percpu.h>

#define PREEMPT_CPU_LIMIT UINT32_C(16)
#define PREEMPT_NESTING_LIMIT UINT32_C(65535)

enum preempt_status {
    PREEMPT_STATUS_OK = 0,
    PREEMPT_STATUS_NULL_ARGUMENT,
    PREEMPT_STATUS_ALREADY_INITIALIZED,
    PREEMPT_STATUS_NOT_INITIALIZED,
    PREEMPT_STATUS_POISONED,
    PREEMPT_STATUS_INVALID_CPU,
    PREEMPT_STATUS_CPU_ALREADY_ONLINE,
    PREEMPT_STATUS_CPU_OFFLINE,
    PREEMPT_STATUS_LAST_CPU,
    PREEMPT_STATUS_CPU_BUSY,
    PREEMPT_STATUS_COUNTER_OVERFLOW,
    PREEMPT_STATUS_COUNTER_UNDERFLOW,
    PREEMPT_STATUS_NOT_ELIGIBLE,
    PREEMPT_STATUS_CORRUPTED
};

struct preempt_cpu_snapshot {
    uint32_t irq_depth;
    uint32_t preempt_count;
    bool need_resched;
    bool online;
};

struct preempt_guard {
    struct percpu_handle cpu;
    bool restore_interrupts;
    bool active;
    bool accounted;
    bool runtime_present;
};

enum preempt_status preempt_runtime_initialize(void);
enum preempt_status preempt_runtime_cpu_online(uint32_t cpu_index);
enum preempt_status preempt_runtime_cpu_offline(uint32_t cpu_index);
enum preempt_status preempt_runtime_irq_enter(void);
enum preempt_status preempt_runtime_irq_exit(bool *reschedule_eligible);
enum preempt_status preempt_runtime_request_reschedule(void);
enum preempt_status preempt_runtime_take_reschedule(void);
enum preempt_status preempt_runtime_switch_allowed(bool *allowed);
enum preempt_status preempt_guard_enter(struct preempt_guard *guard);
enum preempt_status preempt_guard_exit(struct preempt_guard *guard);
enum preempt_status preempt_runtime_snapshot(struct preempt_cpu_snapshot *snapshot);
enum preempt_status preempt_runtime_cpu_snapshot(
    uint32_t cpu_index,
    struct preempt_cpu_snapshot *snapshot
);
const char *preempt_status_string(enum preempt_status status);

#endif

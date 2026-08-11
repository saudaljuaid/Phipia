/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_PERCPU_CORE_H
#define ZENITH_PERCPU_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zenith/percpu.h>

struct percpu_core_cpu {
    uint32_t apic_id;
    uint32_t generation;
    uint8_t online;
    uint8_t boot_cpu;
    uint8_t reserved[2];
};

struct percpu_core_state {
    struct percpu_core_cpu cpus[PERCPU_CPU_LIMIT];
    uint32_t online_count;
    uint32_t next_generation;
    uint32_t boot_cpu_index;
    uint8_t initialized;
    uint8_t poisoned;
    uint8_t reserved[2];
};

/* Clear storage before its first initialize call; initialize reads the
 * initialized marker so that a second initialization is rejected. */
void percpu_core_clear(struct percpu_core_state *state);
enum percpu_status percpu_core_initialize(
    struct percpu_core_state *state,
    uint32_t boot_cpu_index,
    uint32_t boot_apic_id
);
enum percpu_status percpu_core_validate(const struct percpu_core_state *state);
enum percpu_status percpu_core_cpu_online(
    struct percpu_core_state *state,
    uint32_t cpu_index,
    uint32_t apic_id
);
enum percpu_status percpu_core_cpu_offline(
    struct percpu_core_state *state,
    uint32_t cpu_index
);
enum percpu_status percpu_core_lookup_apic(
    const struct percpu_core_state *state,
    uint32_t apic_id,
    uint32_t *cpu_index
);
enum percpu_status percpu_core_handle(
    const struct percpu_core_state *state,
    uint32_t cpu_index,
    struct percpu_handle *handle
);
enum percpu_status percpu_core_resolve(
    const struct percpu_core_state *state,
    const struct percpu_handle *handle,
    uint32_t *cpu_index
);
enum percpu_status percpu_core_snapshot(
    const struct percpu_core_state *state,
    uint32_t cpu_index,
    struct percpu_snapshot *snapshot
);
bool percpu_core_is_online(
    const struct percpu_core_state *state,
    uint32_t cpu_index
);

/* Runtime-private bridge used by the irqsave lock validator. */
const struct percpu_core_state *percpu_runtime_core_state(void);
bool percpu_runtime_registry_lock(void);
void percpu_runtime_registry_unlock(void);

#endif

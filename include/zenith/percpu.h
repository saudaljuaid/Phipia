/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_PERCPU_H
#define ZENITH_PERCPU_H

#include <stdbool.h>
#include <stdint.h>

#define PERCPU_CPU_LIMIT UINT32_C(16)
#define PERCPU_INVALID_CPU UINT32_MAX
#define PERCPU_INVALID_APIC_ID UINT32_MAX

enum percpu_status {
    PERCPU_STATUS_OK = 0,
    PERCPU_STATUS_NULL_ARGUMENT,
    PERCPU_STATUS_ALREADY_INITIALIZED,
    PERCPU_STATUS_NOT_INITIALIZED,
    PERCPU_STATUS_POISONED,
    PERCPU_STATUS_INVALID_CPU,
    PERCPU_STATUS_INVALID_APIC_ID,
    PERCPU_STATUS_CPU_ALREADY_ONLINE,
    PERCPU_STATUS_CPU_OFFLINE,
    PERCPU_STATUS_DUPLICATE_APIC_ID,
    PERCPU_STATUS_BOOT_CPU,
    PERCPU_STATUS_GENERATION_EXHAUSTED,
    PERCPU_STATUS_STALE_HANDLE,
    PERCPU_STATUS_CORRUPTED
};

struct percpu_handle {
    uint32_t cpu_index;
    uint32_t generation;
};

struct percpu_snapshot {
    uint32_t apic_id;
    uint32_t generation;
    bool online;
    bool boot_cpu;
};

/*
 * The runtime currently brings up only the bootstrap processor, but the
 * registry and lookup ABI are deliberately bounded for CPU indices 0..15.
 * A future AP must be registered before it enables interrupts or enters the
 * scheduler.  x2APIC is not enabled by Zenith yet, so the current hardware ID
 * is the initial 8-bit APIC ID reported by CPUID leaf 1.
 */
enum percpu_status percpu_runtime_initialize(
    uint32_t boot_cpu_index,
    uint32_t boot_apic_id
);
enum percpu_status percpu_runtime_initialize_current(uint32_t boot_cpu_index);
enum percpu_status percpu_runtime_register_cpu(
    uint32_t cpu_index,
    uint32_t apic_id,
    struct percpu_handle *handle
);
enum percpu_status percpu_runtime_current_cpu(uint32_t *cpu_index);
enum percpu_status percpu_runtime_current_handle(struct percpu_handle *handle);
enum percpu_status percpu_runtime_resolve(
    const struct percpu_handle *handle,
    uint32_t *cpu_index
);
enum percpu_status percpu_runtime_snapshot(
    uint32_t cpu_index,
    struct percpu_snapshot *snapshot
);
enum percpu_status percpu_runtime_validate(void);
bool percpu_runtime_ready(void);
const char *percpu_status_string(enum percpu_status status);

#endif

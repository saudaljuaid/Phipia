/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_XSTATE_H
#define ZENITH_XSTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XSTATE_TASK_LIMIT UINT32_C(16)
#define XSTATE_CPU_LIMIT UINT32_C(16)
#define XSTATE_INVALID_INDEX UINT32_MAX
#define XSTATE_AREA_ALIGNMENT UINT32_C(64)
#define XSTATE_LEGACY_AREA_SIZE UINT32_C(512)
#define XSTATE_HEADER_SIZE UINT32_C(64)
#define XSTATE_MINIMUM_AREA_SIZE \
    (XSTATE_LEGACY_AREA_SIZE + XSTATE_HEADER_SIZE)
#define XSTATE_MAXIMUM_AREA_SIZE UINT32_C(4096)

#define XSTATE_X87_MASK (UINT64_C(1) << 0U)
#define XSTATE_SSE_MASK (UINT64_C(1) << 1U)
#define XSTATE_AVX_MASK (UINT64_C(1) << 2U)
#define XSTATE_BOUNDED_MASK \
    (XSTATE_X87_MASK | XSTATE_SSE_MASK | XSTATE_AVX_MASK)

enum xstate_task_state {
    XSTATE_TASK_FREE = 0,
    XSTATE_TASK_BUILDING,
    XSTATE_TASK_LIVE,
    XSTATE_TASK_DYING,
    XSTATE_TASK_RETIRED
};

enum xstate_status {
    XSTATE_STATUS_OK = 0,
    XSTATE_STATUS_NULL_ARGUMENT,
    XSTATE_STATUS_ALREADY_INITIALIZED,
    XSTATE_STATUS_NOT_INITIALIZED,
    XSTATE_STATUS_POISONED,
    XSTATE_STATUS_INVALID_CPU,
    XSTATE_STATUS_CPU_ALREADY_ONLINE,
    XSTATE_STATUS_CPU_OFFLINE,
    XSTATE_STATUS_CPU_BUSY,
    XSTATE_STATUS_LAST_CPU,
    XSTATE_STATUS_XSAVE_UNAVAILABLE,
    XSTATE_STATUS_OSXSAVE_UNAVAILABLE,
    XSTATE_STATUS_CPUID_LEAF_UNAVAILABLE,
    XSTATE_STATUS_INVALID_XCR0,
    XSTATE_STATUS_UNSUPPORTED_FEATURE,
    XSTATE_STATUS_INVALID_AREA_SIZE,
    XSTATE_STATUS_INVALID_COMPONENT,
    XSTATE_STATUS_MISALIGNED_AREA,
    XSTATE_STATUS_AREA_TOO_SMALL,
    XSTATE_STATUS_NO_TASK_SLOT,
    XSTATE_STATUS_INVALID_HANDLE,
    XSTATE_STATUS_STALE_HANDLE,
    XSTATE_STATUS_GENERATION_EXHAUSTED,
    XSTATE_STATUS_INVALID_STATE,
    XSTATE_STATUS_IMAGE_NOT_READY,
    XSTATE_STATUS_IMAGE_ALREADY_READY,
    XSTATE_STATUS_CPU_ALREADY_LOADED,
    XSTATE_STATUS_TASK_ALREADY_LOADED,
    XSTATE_STATUS_TASK_NOT_LOADED,
    XSTATE_STATUS_OWNER_MISMATCH,
    XSTATE_STATUS_COUNTER_EXHAUSTED,
    XSTATE_STATUS_UNEXPECTED_NM,
    XSTATE_STATUS_CORRUPTED
};

/* Values in this probe are the sanitized results of CPUID.01H, XGETBV(0),
 * CPUID.0DH:0 and CPUID.0DH:2. The pure core never executes CPUID or XGETBV. */
struct xstate_layout_probe {
    uint64_t cpuid_xcr0_mask;
    uint64_t xcr0;
    uint32_t enabled_area_size;
    uint32_t maximum_area_size;
    uint32_t avx_component_size;
    uint32_t avx_component_offset;
    uint8_t xsave;
    uint8_t osxsave;
    uint8_t leaf_d;
    uint8_t reserved;
};

struct xstate_layout {
    uint64_t enabled_mask;
    uint64_t bounded_hardware_mask;
    uint32_t area_size;
    uint32_t alignment;
    uint32_t avx_component_size;
    uint32_t avx_component_offset;
};

struct xstate_task_handle {
    uint32_t index;
    uint32_t generation;
};

struct xstate_task_snapshot {
    enum xstate_task_state state;
    uint32_t generation;
    uint32_t owner_cpu;
    uint64_t save_count;
    uint64_t restore_count;
    bool image_ready;
};

struct xstate_cpu_snapshot {
    uint32_t loaded_task;
    uint32_t loaded_generation;
    bool online;
};

struct xstate_runtime_snapshot {
    struct xstate_layout layout;
    uint32_t live_tasks;
    uint32_t loaded_task;
    uint32_t loaded_generation;
    uint64_t bootstrap_save_count;
    uint64_t bootstrap_restore_count;
    bool bootstrap_loaded;
    bool initialized;
    bool poisoned;
};

bool xstate_runtime_is_initialized(void);
enum xstate_status xstate_runtime_validate(void);
enum xstate_status xstate_runtime_snapshot(
    struct xstate_runtime_snapshot *snapshot
);
enum xstate_status xstate_runtime_unexpected_nm(void);
const char *xstate_status_string(enum xstate_status status);

#endif

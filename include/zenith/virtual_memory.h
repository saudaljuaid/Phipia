/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_VIRTUAL_MEMORY_H
#define ZENITH_VIRTUAL_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>

#define VIRTUAL_MEMORY_DEVICE_WINDOW_BASE UINT64_C(0xFFFF800000000000)
#define VIRTUAL_MEMORY_HEAP_BASE UINT64_C(0xFFFF900000000000)
#define VIRTUAL_MEMORY_HEAP_SIZE (UINT64_C(16) * 1024U * 1024U)
#define VIRTUAL_MEMORY_HEAP_GUARD_BELOW \
    (VIRTUAL_MEMORY_HEAP_BASE - UINT64_C(4096))
#define VIRTUAL_MEMORY_HEAP_GUARD_ABOVE \
    (VIRTUAL_MEMORY_HEAP_BASE + VIRTUAL_MEMORY_HEAP_SIZE)
#define VIRTUAL_MEMORY_TASK_STACK_BASE UINT64_C(0xFFFFA00000000000)
#define VIRTUAL_MEMORY_TASK_STACK_LIMIT ((size_t)16U)
#define VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES ((size_t)16U)
#define VIRTUAL_MEMORY_TASK_STACK_SLOT_PAGES \
    (VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES + (size_t)2U)
#define VIRTUAL_MEMORY_TASK_STACK_SLOT_SIZE \
    ((uint64_t)VIRTUAL_MEMORY_TASK_STACK_SLOT_PAGES * UINT64_C(4096))
#define VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE \
    ((uint64_t)VIRTUAL_MEMORY_TASK_STACK_LIMIT * \
        VIRTUAL_MEMORY_TASK_STACK_SLOT_SIZE)
#define VIRTUAL_MEMORY_TASK_STACK_WINDOW_END \
    (VIRTUAL_MEMORY_TASK_STACK_BASE + VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE)

enum virtual_memory_status {
    VIRTUAL_MEMORY_STATUS_OK = 0,
    VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT,
    VIRTUAL_MEMORY_STATUS_ALREADY_INITIALIZED,
    VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED,
    VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED,
    VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU,
    VIRTUAL_MEMORY_STATUS_BAD_KERNEL_LAYOUT,
    VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT,
    VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS,
    VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE,
    VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW,
    VIRTUAL_MEMORY_STATUS_RANGE_TOO_LARGE,
    VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS,
    VIRTUAL_MEMORY_STATUS_TABLE_LIMIT,
    VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT,
    VIRTUAL_MEMORY_STATUS_NOT_MAPPED,
    VIRTUAL_MEMORY_STATUS_OUTSIDE_HEAP_WINDOW,
    VIRTUAL_MEMORY_STATUS_OUTSIDE_TASK_STACK_WINDOW,
    VIRTUAL_MEMORY_STATUS_BAD_STACK_SLOT,
    VIRTUAL_MEMORY_STATUS_BAD_STACK_PAGE,
    VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH,
    VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE,
    VIRTUAL_MEMORY_STATUS_TEST_FAILURE,
    VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS,
    VIRTUAL_MEMORY_STATUS_TOO_MANY_DEVICES,
    VIRTUAL_MEMORY_STATUS_PAT_FAILURE,
    VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE
};

enum virtual_memory_permissions {
    VIRTUAL_MEMORY_READ_ONLY = 0,
    VIRTUAL_MEMORY_WRITABLE = 1U << 0,
    VIRTUAL_MEMORY_EXECUTABLE = 1U << 1,
    VIRTUAL_MEMORY_DEVICE = 1U << 2
};

struct virtual_memory_mapping {
    uint64_t physical_address;
    uint32_t permissions;
};

struct virtual_memory_layout {
    uint64_t root_table_address;
    uint64_t local_apic_virtual_address;
    uint64_t io_apic_virtual_addresses[ACPI_MAX_IO_APICS];
    size_t io_apic_count;
    size_t table_page_count;
};

struct virtual_memory_runtime_stats {
    size_t heap_window_pages;
    size_t heap_mapped_pages;
    size_t task_stack_payload_pages;
    size_t task_stack_mapped_pages;
    size_t table_pages_used;
    size_t table_pages_capacity;
};

struct virtual_memory_task_stack_certificate {
    uint64_t mutation_epoch;
    size_t mapped_pages;
};

bool virtual_memory_self_test(void);
enum virtual_memory_status virtual_memory_initialize(
    const struct acpi_topology *topology,
    struct virtual_memory_layout *layout
);
enum virtual_memory_status virtual_memory_validate(void);
enum virtual_memory_status virtual_memory_query(
    uint64_t virtual_address,
    struct virtual_memory_mapping *mapping
);
enum virtual_memory_status virtual_memory_map_heap_page(
    uint64_t virtual_address,
    uintptr_t physical_address
);
enum virtual_memory_status virtual_memory_unmap_heap_page(
    uint64_t virtual_address,
    uintptr_t expected_physical_address
);
enum virtual_memory_status virtual_memory_task_stack_bounds(
    size_t slot_index,
    uint64_t *lower_guard,
    uint64_t *payload_start,
    uint64_t *payload_end,
    uint64_t *upper_guard
);
enum virtual_memory_status virtual_memory_query_task_stack_page(
    size_t slot_index,
    size_t page_index,
    struct virtual_memory_mapping *mapping
);
enum virtual_memory_status virtual_memory_map_task_stack_page(
    size_t slot_index,
    size_t page_index,
    uintptr_t physical_address
);
enum virtual_memory_status virtual_memory_unmap_task_stack_page(
    size_t slot_index,
    size_t page_index,
    uintptr_t expected_physical_address
);
enum virtual_memory_status virtual_memory_runtime_get_stats(
    struct virtual_memory_runtime_stats *stats
);
/*
 * Validate the complete active address space and prove that the heap window
 * is exactly the contiguous union of these two frame spans.  Every heap leaf,
 * including every required-unmapped leaf, is inspected under one VM lock.
 * Successful return also supplies the runtime statistics from that same
 * validation snapshot; failure always clears a non-null stats output.
 */
enum virtual_memory_status virtual_memory_validate_heap_backing(
    const uintptr_t *committed_frames,
    size_t committed_page_count,
    const uintptr_t *staged_frames,
    size_t staged_page_count,
    struct virtual_memory_runtime_stats *stats
);
/* O(1), irqsave-locked certificate; every failure clears the output. */
enum virtual_memory_status virtual_memory_task_stack_certificate_get(
    struct virtual_memory_task_stack_certificate *certificate
);
bool virtual_memory_ready(void);
const char *virtual_memory_status_string(enum virtual_memory_status status);

#endif

/* SPDX-License-Identifier: GPL-3.0-only */
#include <zenith/cpu.h>
#include <zenith/spinlock.h>
#include <zenith/test.h>
#include <zenith/virtual_memory.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef VM_EPOCH_RUNNER
uint8_t __kernel_start[1];
uint8_t __kernel_end[1];
uint8_t __text_start[1];
uint8_t __text_end[1];
uint8_t __rodata_start[1];
uint8_t __rodata_end[1];
uint8_t __data_start[1];
uint8_t __data_end[1];
uint8_t __bss_start[1];
uint8_t __bss_end[1];
#endif

static struct spinlock *registered_lock;
static bool interrupts_enabled;
static bool fail_next_release;
static size_t acquire_count;
static size_t register_count;
static uint64_t next_generation = UINT64_C(1);
static uintptr_t emulated_cr3;
static bool fail_next_post_invalidate_validation;
static size_t bad_cr0_reads_remaining;

void cpu_interrupt_disable(void)
{
    interrupts_enabled = false;
}

void cpu_interrupt_enable(void)
{
    interrupts_enabled = true;
}

bool cpu_interrupts_enabled(void)
{
    return interrupts_enabled;
}

uint64_t cpu_read_cr0(void)
{
    if (bad_cr0_reads_remaining != 0U) {
        --bad_cr0_reads_remaining;
        return 0U;
    }
    return UINT64_C(1) << 16U;
}

uintptr_t cpu_read_cr3(void)
{
    return emulated_cr3;
}

void cpu_write_cr0(uint64_t value)
{
    (void)value;
}

void cpu_write_cr3(uintptr_t physical_address)
{
    emulated_cr3 = physical_address;
}

void cpu_invalidate_page(uintptr_t virtual_address)
{
    (void)virtual_address;
    if (fail_next_post_invalidate_validation) {
        fail_next_post_invalidate_validation = false;
        bad_cr0_reads_remaining = 1U;
    }
}

uint64_t cpu_read_msr(uint32_t msr)
{
    if (msr == UINT32_C(0xC0000080)) {
        return UINT64_C(1) << 11U;
    }
    return msr == UINT32_C(0x00000277)
        ? UINT64_C(0x0007040600070406)
        : 0U;
}

void cpu_write_msr(uint32_t msr, uint64_t value)
{
    (void)msr;
    (void)value;
}

void cpu_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    struct cpu_cpuid_result *result
)
{
    (void)subleaf;
    if (result != NULL) {
        const struct cpu_cpuid_result zero = {0U, 0U, 0U, 0U};

        *result = zero;
        if (leaf == UINT32_C(1)) {
            result->edx = (UINT32_C(1) << 5U) | (UINT32_C(1) << 16U);
        } else if (leaf == UINT32_C(0x80000000)) {
            result->eax = UINT32_C(0x80000008);
        } else if (leaf == UINT32_C(0x80000001)) {
            result->edx = UINT32_C(1) << 20U;
        } else if (leaf == UINT32_C(0x80000008)) {
            result->eax = 52U;
        }
    }
}

void cpu_write_back_and_invalidate(void)
{
}

enum spinlock_status spinlock_runtime_register(
    struct spinlock *lock,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
)
{
    if (lock == NULL || registered_lock != NULL || lock_index != 3U ||
        order_class != SPINLOCK_CLASS_VIRTUAL_MEMORY || order_rank != 0U) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }

    lock->word = 0U;
    lock->lock_index = lock_index;
    lock->registered = 1U;
    registered_lock = lock;
    ++register_count;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_irqsave_acquire(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    if (lock == NULL || token == NULL || lock != registered_lock ||
        lock->registered == 0U) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }
    if (lock->word != 0U) {
        return SPINLOCK_STATUS_LOCK_BUSY;
    }

    token->cpu_index = 0U;
    token->lock_index = lock->lock_index;
    token->generation = next_generation++;
    token->interrupts_were_enabled = interrupts_enabled;
    interrupts_enabled = false;
    lock->word = 1U;
    ++acquire_count;
    return SPINLOCK_STATUS_OK;
}

enum spinlock_status spinlock_irqrestore_release(
    struct spinlock *lock,
    struct spinlock_irq_token *token
)
{
    if (lock == NULL || token == NULL || lock != registered_lock ||
        lock->word == 0U || token->lock_index != lock->lock_index) {
        interrupts_enabled = false;
        return SPINLOCK_STATUS_CORRUPTED;
    }
    if (fail_next_release) {
        fail_next_release = false;
        interrupts_enabled = false;
        return SPINLOCK_STATUS_CORRUPTED;
    }

    lock->word = 0U;
    interrupts_enabled = token->interrupts_were_enabled;
    token->lock_index = UINT32_MAX;
    return SPINLOCK_STATUS_OK;
}

static bool mapping_is_zero(const struct virtual_memory_mapping *mapping)
{
    return mapping->physical_address == 0U && mapping->permissions == 0U;
}

static bool layout_is_zero(const struct virtual_memory_layout *layout)
{
    if (layout->root_table_address != 0U ||
        layout->local_apic_virtual_address != 0U ||
        layout->io_apic_count != 0U || layout->table_page_count != 0U) {
        return false;
    }
    for (size_t index = 0U; index < ACPI_MAX_IO_APICS; ++index) {
        if (layout->io_apic_virtual_addresses[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool stats_are_zero(const struct virtual_memory_runtime_stats *stats)
{
    return stats->heap_window_pages == 0U &&
        stats->heap_mapped_pages == 0U &&
        stats->task_stack_payload_pages == 0U &&
        stats->task_stack_mapped_pages == 0U &&
        stats->table_pages_used == 0U && stats->table_pages_capacity == 0U;
}

static bool scenario_exact_if(void)
{
    const struct acpi_topology topology = {0};
    struct virtual_memory_layout layout;
    struct virtual_memory_mapping mapping = {UINT64_MAX, UINT32_MAX};
    struct virtual_memory_runtime_stats stats = {
        SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX
    };
    struct virtual_memory_runtime_stats batch_stats = {
        SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX
    };
    struct virtual_memory_task_stack_certificate certificate = {
        UINT64_MAX, SIZE_MAX
    };

    memset(&layout, 0xA5, sizeof(layout));
    interrupts_enabled = true;
    return virtual_memory_initialize(&topology, &layout) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        layout_is_zero(&layout) && interrupts_enabled && register_count == 0U &&
        virtual_memory_validate() ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        virtual_memory_query(VIRTUAL_MEMORY_HEAP_BASE, &mapping) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        mapping_is_zero(&mapping) &&
        virtual_memory_query_task_stack_page(0U, 0U, &mapping) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        mapping_is_zero(&mapping) &&
        virtual_memory_map_heap_page(VIRTUAL_MEMORY_HEAP_BASE, 0U) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        virtual_memory_unmap_heap_page(VIRTUAL_MEMORY_HEAP_BASE, 0U) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        virtual_memory_map_task_stack_page(0U, 0U, 0U) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        virtual_memory_unmap_task_stack_page(0U, 0U, 0U) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        virtual_memory_runtime_get_stats(&stats) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        stats_are_zero(&stats) &&
        virtual_memory_validate_heap_backing(
            NULL,
            0U,
            NULL,
            0U,
            &batch_stats
        ) == VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        stats_are_zero(&batch_stats) &&
        virtual_memory_task_stack_certificate_get(&certificate) ==
            VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        certificate.mutation_epoch == 0U && certificate.mapped_pages == 0U &&
        !virtual_memory_test_fail_heap_map_after(0U) &&
        !virtual_memory_test_report_uncertain_heap_map_once() &&
        !virtual_memory_test_fail_task_stack_map_after(0U) &&
        !virtual_memory_test_report_uncertain_task_stack_map_once() &&
        !virtual_memory_test_set_task_stack_mutation_epoch(UINT64_MAX) &&
        !virtual_memory_ready() && interrupts_enabled && acquire_count == 0U;
}

static bool register_with_if_clear(void)
{
    const struct acpi_topology topology = {0};
    struct virtual_memory_layout layout;
    enum virtual_memory_status status;

    interrupts_enabled = false;
    status = virtual_memory_initialize(&topology, &layout);
    return status != VIRTUAL_MEMORY_STATUS_OK &&
        status != VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED &&
        register_count == 1U && acquire_count == 1U &&
        registered_lock != NULL && registered_lock->word == 0U &&
        !interrupts_enabled && layout_is_zero(&layout);
}

static bool scenario_if_clear(void)
{
    struct virtual_memory_mapping mapping = {UINT64_MAX, UINT32_MAX};
    struct virtual_memory_runtime_stats stats = {
        SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX
    };
    struct virtual_memory_task_stack_certificate certificate = {
        UINT64_MAX, SIZE_MAX
    };

    return register_with_if_clear() &&
        virtual_memory_validate() == VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED &&
        !interrupts_enabled && acquire_count == 2U &&
        virtual_memory_validate_heap_backing(
            NULL,
            0U,
            NULL,
            0U,
            &stats
        ) == VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED &&
        stats_are_zero(&stats) && !interrupts_enabled && acquire_count == 3U &&
        virtual_memory_task_stack_certificate_get(&certificate) ==
            VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED &&
        certificate.mutation_epoch == 0U && certificate.mapped_pages == 0U &&
        !interrupts_enabled && acquire_count == 4U &&
        virtual_memory_query(VIRTUAL_MEMORY_HEAP_BASE, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED &&
        mapping_is_zero(&mapping) && !interrupts_enabled &&
        acquire_count == 5U && registered_lock->word == 0U;
}

static bool scenario_poison(void)
{
    struct virtual_memory_mapping mapping = {UINT64_MAX, UINT32_MAX};
    struct virtual_memory_runtime_stats stats = {
        SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX
    };
    struct virtual_memory_task_stack_certificate certificate = {
        UINT64_MAX, SIZE_MAX
    };

    if (!register_with_if_clear()) {
        return false;
    }
    fail_next_release = true;
    return virtual_memory_query(VIRTUAL_MEMORY_HEAP_BASE, &mapping) ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        mapping_is_zero(&mapping) && !interrupts_enabled &&
        registered_lock->word != 0U &&
        virtual_memory_validate() ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        virtual_memory_validate_heap_backing(
            NULL,
            0U,
            NULL,
            0U,
            &stats
        ) == VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        stats_are_zero(&stats) &&
        virtual_memory_task_stack_certificate_get(&certificate) ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        certificate.mutation_epoch == 0U && certificate.mapped_pages == 0U;
}

#ifdef VM_EPOCH_RUNNER
static bool initialize_epoch_vm(void)
{
    const struct acpi_topology topology = {
        .local_apic_address = UINT64_C(0xFEE00000),
        .io_apic_count = 0U
    };
    struct virtual_memory_layout layout;

    interrupts_enabled = false;
    return virtual_memory_initialize(&topology, &layout) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        layout.root_table_address != 0U &&
        virtual_memory_validate() == VIRTUAL_MEMORY_STATUS_OK;
}

static bool scenario_epoch_map_overflow(uint64_t mutation_epoch)
{
    const uintptr_t frame = UINT64_C(0x00200000);

    return initialize_epoch_vm() &&
        virtual_memory_test_set_task_stack_mutation_epoch(mutation_epoch) &&
        virtual_memory_map_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        virtual_memory_test_task_stack_state_matches(
            0U,
            0U,
            false,
            0U,
            0U
        );
}

static bool scenario_epoch_uncertain_cleanup(void)
{
    const uintptr_t frame = UINT64_C(0x00200000);
    struct virtual_memory_task_stack_certificate mapped = {0U};
    struct virtual_memory_task_stack_certificate unmapped = {0U};

    return initialize_epoch_vm() &&
        virtual_memory_test_set_task_stack_mutation_epoch(UINT64_MAX - 3U) &&
        virtual_memory_test_report_uncertain_task_stack_map_once() &&
        virtual_memory_map_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE &&
        virtual_memory_test_task_stack_state_matches(
            0U,
            0U,
            true,
            frame,
            1U
        ) &&
        virtual_memory_task_stack_certificate_get(&mapped) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        mapped.mutation_epoch == UINT64_MAX - 2U &&
        mapped.mapped_pages == 1U &&
        virtual_memory_unmap_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        virtual_memory_task_stack_certificate_get(&unmapped) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        unmapped.mutation_epoch == UINT64_MAX - 1U &&
        unmapped.mapped_pages == 0U;
}

static bool scenario_epoch_map_restore(void)
{
    const uintptr_t frame = UINT64_C(0x00200000);
    struct virtual_memory_task_stack_certificate certificate = {0U};

    if (!initialize_epoch_vm() ||
        !virtual_memory_test_set_task_stack_mutation_epoch(UINT64_C(10))) {
        return false;
    }
    fail_next_post_invalidate_validation = true;
    return virtual_memory_map_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        virtual_memory_test_task_stack_state_matches(
            0U,
            0U,
            false,
            0U,
            0U
        ) &&
        virtual_memory_task_stack_certificate_get(&certificate) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        certificate.mutation_epoch == UINT64_C(12) &&
        certificate.mapped_pages == 0U;
}

static bool scenario_epoch_unmap_restore(void)
{
    const uintptr_t frame = UINT64_C(0x00200000);
    struct virtual_memory_task_stack_certificate before = {0U};
    struct virtual_memory_task_stack_certificate restored = {0U};

    if (!initialize_epoch_vm() ||
        virtual_memory_map_task_stack_page(0U, 0U, frame) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        virtual_memory_task_stack_certificate_get(&before) !=
            VIRTUAL_MEMORY_STATUS_OK) {
        return false;
    }
    fail_next_post_invalidate_validation = true;
    return virtual_memory_unmap_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE &&
        virtual_memory_test_task_stack_state_matches(
            0U,
            0U,
            true,
            frame,
            1U
        ) &&
        virtual_memory_task_stack_certificate_get(&restored) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        restored.mutation_epoch == before.mutation_epoch + 2U &&
        restored.mapped_pages == 1U &&
        virtual_memory_unmap_task_stack_page(0U, 0U, frame) ==
            VIRTUAL_MEMORY_STATUS_OK;
}

static bool scenario_heap_map_roundtrip(void)
{
    const uintptr_t frame = UINT64_C(0x00200000);
    struct virtual_memory_mapping mapping = {UINT64_MAX, UINT32_MAX};

    return initialize_epoch_vm() &&
        virtual_memory_map_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        virtual_memory_validate() == VIRTUAL_MEMORY_STATUS_OK &&
        virtual_memory_unmap_heap_page(VIRTUAL_MEMORY_HEAP_BASE, frame) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        virtual_memory_query(VIRTUAL_MEMORY_HEAP_BASE, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED &&
        mapping.physical_address == 0U && mapping.permissions == 0U &&
        virtual_memory_validate() == VIRTUAL_MEMORY_STATUS_OK;
}
#endif

int main(int argument_count, char **arguments)
{
    bool passed = false;

    if (argument_count != 2) {
        return 2;
    }
    if (strcmp(arguments[1], "exact-if") == 0) {
        passed = scenario_exact_if();
    } else if (strcmp(arguments[1], "if-clear") == 0) {
        passed = scenario_if_clear();
    } else if (strcmp(arguments[1], "poison") == 0) {
        passed = scenario_poison();
#ifdef VM_EPOCH_RUNNER
    } else if (strcmp(arguments[1], "epoch-map-max-2") == 0) {
        passed = scenario_epoch_map_overflow(UINT64_MAX - 2U);
    } else if (strcmp(arguments[1], "epoch-map-max-1") == 0) {
        passed = scenario_epoch_map_overflow(UINT64_MAX - 1U);
    } else if (strcmp(arguments[1], "epoch-map-max") == 0) {
        passed = scenario_epoch_map_overflow(UINT64_MAX);
    } else if (strcmp(arguments[1], "epoch-uncertain-cleanup") == 0) {
        passed = scenario_epoch_uncertain_cleanup();
    } else if (strcmp(arguments[1], "epoch-map-restore") == 0) {
        passed = scenario_epoch_map_restore();
    } else if (strcmp(arguments[1], "epoch-unmap-restore") == 0) {
        passed = scenario_epoch_unmap_restore();
    } else if (strcmp(arguments[1], "heap-map-roundtrip") == 0) {
        passed = scenario_heap_map_roundtrip();
#endif
    }
    if (!passed) {
        fputs("virtual-memory irqsave scenario failed\n", stderr);
        return 1;
    }

    puts("virtual-memory irqsave scenario passed");
    return 0;
}

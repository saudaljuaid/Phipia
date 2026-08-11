/* SPDX-License-Identifier: GPL-3.0-only */
#include <zenith/boot.h>
#include <zenith/memory.h>
#include <zenith/spinlock.h>
#include <zenith/test.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_AVAILABLE_BYTES UINT64_C(0x04000000)
#define TEST_RESERVE_ADDRESS UINT64_C(0x00300000)

bool frame_test_set_process_page_mutation_epoch(uint64_t mutation_epoch);
bool frame_test_process_page_state_matches(size_t expected_owned_frames);

static struct spinlock *registered_lock;
static bool interrupts_enabled;
static bool fail_next_lock_release;
static uint64_t next_generation = UINT64_C(1);
static const struct multiboot2_memory_map_tag test_memory_map = {
    .tag = {
        .type = MULTIBOOT2_TAG_MEMORY_MAP,
        .size = sizeof(struct multiboot2_memory_map_tag)
    },
    .entry_size = sizeof(struct multiboot2_memory_map_entry),
    .entry_version = 0U
};

bool boot_memory_region_at(
    const struct boot_context *context,
    size_t index,
    struct boot_memory_region *region
)
{
    if (context == NULL || region == NULL || index != 0U ||
        context->memory_map_entry_count != 1U) {
        return false;
    }

    region->base_address = 0U;
    region->length = TEST_AVAILABLE_BYTES;
    region->type = MULTIBOOT2_MEMORY_AVAILABLE;
    return true;
}

enum spinlock_status spinlock_runtime_register(
    struct spinlock *lock,
    uint32_t lock_index,
    enum spinlock_order_class order_class,
    uint8_t order_rank
)
{
    if (lock == NULL || lock_index != UINT32_C(4) ||
        order_class != SPINLOCK_CLASS_PHYSICAL_MEMORY || order_rank != 0U ||
        registered_lock != NULL) {
        return SPINLOCK_STATUS_INVALID_LOCK;
    }

    lock->word = 0U;
    lock->lock_index = lock_index;
    lock->registered = 1U;
    registered_lock = lock;
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
    if (fail_next_lock_release) {
        fail_next_lock_release = false;
        interrupts_enabled = false;
        return SPINLOCK_STATUS_CORRUPTED;
    }

    lock->word = 0U;
    interrupts_enabled = token->interrupts_were_enabled;
    token->lock_index = UINT32_MAX;
    return SPINLOCK_STATUS_OK;
}

static struct boot_context test_context(void)
{
    const struct boot_context context = {
        .information_start = UINT64_C(0x00180000),
        .information_end = UINT64_C(0x00181000),
        .memory_map = &test_memory_map,
        .acpi_old = NULL,
        .acpi_new = NULL,
        .boot_loader_name = NULL,
        .command_line = NULL,
        .boot_loader_name_length = 0U,
        .command_line_length = 0U,
        .memory_map_entry_count = 1U,
        .reported_usable_bytes = TEST_AVAILABLE_BYTES,
        .highest_reported_address = TEST_AVAILABLE_BYTES
    };

    return context;
}

static bool stats_are_zero(struct frame_allocator_stats stats)
{
    return stats.addressable_frames == 0U &&
        stats.allocatable_frames == 0U &&
        stats.free_frames == 0U &&
        stats.allocated_frames == 0U &&
        stats.generic_allocated_frames == 0U &&
        stats.heap_allocated_frames == 0U &&
        stats.task_stack_allocated_frames == 0U &&
        stats.process_page_allocated_frames == 0U &&
        stats.reserved_frames == 0U &&
        stats.highest_allocatable_address == 0U;
}

static bool stats_are_equal(
    const struct frame_allocator_stats *left,
    const struct frame_allocator_stats *right
)
{
    return left->addressable_frames == right->addressable_frames &&
        left->allocatable_frames == right->allocatable_frames &&
        left->free_frames == right->free_frames &&
        left->allocated_frames == right->allocated_frames &&
        left->generic_allocated_frames == right->generic_allocated_frames &&
        left->heap_allocated_frames == right->heap_allocated_frames &&
        left->task_stack_allocated_frames ==
            right->task_stack_allocated_frames &&
        left->process_page_allocated_frames ==
            right->process_page_allocated_frames &&
        left->reserved_frames == right->reserved_frames &&
        left->highest_allocatable_address ==
            right->highest_allocatable_address;
}

static bool certificate_is_zero(
    const struct frame_allocator_task_stack_certificate *certificate
)
{
    return certificate->mutation_epoch == 0U &&
        certificate->owned_frames == 0U;
}

static bool process_certificate_is_zero(
    const struct frame_allocator_process_page_certificate *certificate
)
{
    return certificate->mutation_epoch == 0U &&
        certificate->owned_frames == 0U;
}

static bool process_certificates_are_equal(
    const struct frame_allocator_process_page_certificate *left,
    const struct frame_allocator_process_page_certificate *right
)
{
    return left->mutation_epoch == right->mutation_epoch &&
        left->owned_frames == right->owned_frames;
}

static bool poisoned_surface_is_fail_closed(void)
{
    uintptr_t address = UINTPTR_MAX;
    bool allocated = true;
    enum frame_owner owner = FRAME_OWNER_TASK_STACK;
    struct frame_allocator_task_stack_certificate certificate = {
        UINT64_MAX, SIZE_MAX
    };
    struct frame_allocator_process_page_certificate process_certificate = {
        UINT64_MAX, SIZE_MAX
    };

    return frame_allocator_validate() == FRAME_STATUS_VALIDATION_FAILURE &&
        frame_allocate(&address) == FRAME_STATUS_VALIDATION_FAILURE &&
        address == 0U &&
        frame_release(TEST_RESERVE_ADDRESS) ==
            FRAME_STATUS_VALIDATION_FAILURE &&
        frame_reserve_range(TEST_RESERVE_ADDRESS, ZENITH_PAGE_SIZE) ==
            FRAME_STATUS_VALIDATION_FAILURE &&
        frame_query_allocation(TEST_RESERVE_ADDRESS, &allocated) ==
            FRAME_STATUS_VALIDATION_FAILURE &&
        !allocated &&
        frame_query_owner(TEST_RESERVE_ADDRESS, &owner) ==
            FRAME_STATUS_VALIDATION_FAILURE &&
        owner == FRAME_OWNER_NONE &&
        frame_allocator_task_stack_certificate_get(&certificate) ==
            FRAME_STATUS_VALIDATION_FAILURE &&
        certificate_is_zero(&certificate) &&
        frame_allocator_process_page_certificate_get(
            &process_certificate
        ) == FRAME_STATUS_VALIDATION_FAILURE &&
        process_certificate_is_zero(&process_certificate) &&
        frame_allocator_validate_process_page_owners(
            NULL,
            0U,
            NULL,
            0U,
            &process_certificate
        ) == FRAME_STATUS_VALIDATION_FAILURE &&
        process_certificate_is_zero(&process_certificate) &&
        stats_are_zero(frame_allocator_get_stats()) &&
        !frame_test_fail_allocate_after(0U) &&
        !frame_test_report_out_of_memory_after(0U) &&
        !frame_test_set_process_page_mutation_epoch(1U);
}

static bool initialize_allocator(void)
{
    const struct boot_context context = test_context();

    return frame_allocator_initialize(&context) == FRAME_STATUS_OK &&
        frame_allocator_validate() == FRAME_STATUS_OK &&
        !stats_are_zero(frame_allocator_get_stats());
}

static bool process_owner_proof_fails(
    const uintptr_t *committed_frames,
    size_t committed_frame_count,
    const uintptr_t *staged_frames,
    size_t staged_frame_count,
    enum frame_status expected_status
)
{
    struct frame_allocator_process_page_certificate certificate = {
        UINT64_MAX, SIZE_MAX
    };

    return frame_allocator_validate_process_page_owners(
            committed_frames,
            committed_frame_count,
            staged_frames,
            staged_frame_count,
            &certificate
        ) == expected_status &&
        process_certificate_is_zero(&certificate);
}

static bool run_process_certificate_scenario(void)
{
    uintptr_t process_page = 0U;
    uintptr_t other = 0U;
    enum frame_owner owner = FRAME_OWNER_NONE;
    struct frame_allocator_process_page_certificate before = {0U};
    struct frame_allocator_process_page_certificate unchanged = {0U};
    struct frame_allocator_process_page_certificate allocated = {0U};
    struct frame_allocator_process_page_certificate released = {0U};
    struct frame_allocator_process_page_certificate proof = {0U};
    struct frame_allocator_stats stats;

    interrupts_enabled = true;
    if (frame_allocator_process_page_certificate_get(&before) !=
            FRAME_STATUS_OK ||
        !interrupts_enabled || before.mutation_epoch == 0U ||
        before.owned_frames != 0U) {
        return false;
    }
    interrupts_enabled = false;

    if (frame_allocate(&other) != FRAME_STATUS_OK ||
        frame_allocator_process_page_certificate_get(&unchanged) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&before, &unchanged) ||
        frame_release(other) != FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_KERNEL_HEAP, &other) !=
            FRAME_STATUS_OK ||
        frame_release_owned(other, FRAME_OWNER_KERNEL_HEAP) !=
            FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_TASK_STACK, &other) !=
            FRAME_STATUS_OK ||
        frame_release_owned(other, FRAME_OWNER_TASK_STACK) !=
            FRAME_STATUS_OK ||
        frame_allocator_process_page_certificate_get(&unchanged) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&before, &unchanged) ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &process_page) !=
            FRAME_STATUS_OK ||
        frame_query_owner(process_page, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_PROCESS_PAGE ||
        frame_allocator_process_page_certificate_get(&allocated) !=
            FRAME_STATUS_OK ||
        allocated.mutation_epoch != before.mutation_epoch + 1U ||
        allocated.owned_frames != 1U) {
        return false;
    }

    stats = frame_allocator_get_stats();
    if (stats.process_page_allocated_frames != 1U ||
        stats.allocated_frames != stats.generic_allocated_frames +
            stats.heap_allocated_frames +
            stats.task_stack_allocated_frames +
            stats.process_page_allocated_frames ||
        frame_release_owned(process_page, FRAME_OWNER_GENERIC) !=
            FRAME_STATUS_OWNER_MISMATCH ||
        frame_release_owned(process_page, FRAME_OWNER_KERNEL_HEAP) !=
            FRAME_STATUS_OWNER_MISMATCH ||
        frame_release_owned(process_page, FRAME_OWNER_TASK_STACK) !=
            FRAME_STATUS_OWNER_MISMATCH ||
        frame_allocator_process_page_certificate_get(&unchanged) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&allocated, &unchanged) ||
        frame_release_owned(process_page, FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OK ||
        frame_allocator_process_page_certificate_get(&released) !=
            FRAME_STATUS_OK ||
        released.mutation_epoch != allocated.mutation_epoch + 1U ||
        released.owned_frames != 0U ||
        frame_allocator_validate_process_page_owners(
            NULL,
            0U,
            NULL,
            0U,
            &proof
        ) != FRAME_STATUS_OK ||
        !process_certificates_are_equal(&released, &proof)) {
        return false;
    }

    return frame_allocator_validate() == FRAME_STATUS_OK;
}

static bool run_process_owner_set_scenario(void)
{
    uintptr_t committed[2] = {0U, 0U};
    uintptr_t staged[1] = {0U};
    uintptr_t foreign[3] = {0U, 0U, 0U};
    uintptr_t wrong_committed[2];
    uintptr_t wrong_staged[1];
    struct frame_allocator_process_page_certificate proof = {0U};
    struct frame_allocator_process_page_certificate current = {0U};

    if (frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &committed[0]) !=
            FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &committed[1]) !=
            FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &staged[0]) !=
            FRAME_STATUS_OK ||
        frame_allocate(&foreign[0]) != FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_KERNEL_HEAP, &foreign[1]) !=
            FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_TASK_STACK, &foreign[2]) !=
            FRAME_STATUS_OK) {
        return false;
    }

    interrupts_enabled = true;
    if (frame_allocator_validate_process_page_owners(
            committed,
            2U,
            staged,
            1U,
            &proof
        ) != FRAME_STATUS_OK ||
        !interrupts_enabled || proof.owned_frames != 3U ||
        frame_allocator_process_page_certificate_get(&current) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&proof, &current)) {
        return false;
    }
    interrupts_enabled = false;

    wrong_committed[0] = committed[0];
    wrong_committed[1] = committed[0];
    if (!process_owner_proof_fails(
            wrong_committed, 2U, staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        )) {
        return false;
    }

    wrong_staged[0] = committed[0];
    if (!process_owner_proof_fails(
            committed, 2U, wrong_staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        )) {
        return false;
    }

    for (size_t index = 0U; index < 3U; ++index) {
        wrong_committed[0] = foreign[index];
        wrong_committed[1] = committed[1];
        if (!process_owner_proof_fails(
                wrong_committed,
                2U,
                staged,
                1U,
                FRAME_STATUS_OWNER_MISMATCH
            )) {
            return false;
        }
    }

    wrong_committed[0] = committed[0] | (uintptr_t)1U;
    wrong_committed[1] = committed[1];
    if (!process_owner_proof_fails(
            wrong_committed, 2U, staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        )) {
        return false;
    }
    wrong_committed[0] = (uintptr_t)ZENITH_EARLY_PHYSICAL_LIMIT;
    if (!process_owner_proof_fails(
            wrong_committed, 2U, staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        )) {
        return false;
    }
    wrong_committed[0] = 0U;
    if (!process_owner_proof_fails(
            wrong_committed, 2U, staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        ) ||
        !process_owner_proof_fails(
            NULL, 1U, staged, 1U, FRAME_STATUS_NULL_ARGUMENT
        ) ||
        !process_owner_proof_fails(
            committed, 2U, NULL, 1U, FRAME_STATUS_NULL_ARGUMENT
        ) ||
        !process_owner_proof_fails(
            committed, 1U, staged, 1U, FRAME_STATUS_OWNER_MISMATCH
        ) ||
        !process_owner_proof_fails(
            committed,
            FRAME_ALLOCATOR_PROCESS_PAGE_VALIDATION_LIMIT + 1U,
            NULL,
            0U,
            FRAME_STATUS_OWNER_MISMATCH
        ) ||
        frame_allocator_validate_process_page_owners(
            committed,
            2U,
            staged,
            1U,
            NULL
        ) != FRAME_STATUS_NULL_ARGUMENT ||
        frame_allocator_validate_process_page_owners(
            committed,
            2U,
            staged,
            1U,
            &proof
        ) != FRAME_STATUS_OK ||
        proof.owned_frames != 3U) {
        return false;
    }

    if (frame_release_owned(staged[0], FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OK ||
        frame_release_owned(committed[1], FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OK ||
        frame_release_owned(committed[0], FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OK ||
        frame_release(foreign[0]) != FRAME_STATUS_OK ||
        frame_release_owned(foreign[1], FRAME_OWNER_KERNEL_HEAP) !=
            FRAME_STATUS_OK ||
        frame_release_owned(foreign[2], FRAME_OWNER_TASK_STACK) !=
            FRAME_STATUS_OK ||
        frame_allocator_validate_process_page_owners(
            NULL,
            0U,
            NULL,
            0U,
            &proof
        ) != FRAME_STATUS_OK ||
        proof.owned_frames != 0U) {
        return false;
    }

    return frame_allocator_validate() == FRAME_STATUS_OK;
}

static bool run_process_failure_atomicity_scenario(void)
{
    uintptr_t address = UINTPTR_MAX;
    uintptr_t generic = 0U;
    struct frame_allocator_process_page_certificate before = {0U};
    struct frame_allocator_process_page_certificate after = {0U};
    struct frame_allocator_process_page_certificate allocated = {0U};
    struct frame_allocator_process_page_certificate released = {0U};

    if (frame_allocator_process_page_certificate_get(&before) !=
            FRAME_STATUS_OK ||
        !frame_test_fail_allocate_after(0U) ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) !=
            FRAME_STATUS_TEST_FAILURE ||
        address != 0U ||
        frame_allocator_process_page_certificate_get(&after) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&before, &after) ||
        !frame_test_report_out_of_memory_after(0U) ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) !=
            FRAME_STATUS_OUT_OF_MEMORY ||
        address != 0U ||
        frame_allocator_process_page_certificate_get(&after) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&before, &after) ||
        frame_allocate(&generic) != FRAME_STATUS_OK ||
        frame_release_owned(generic, FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OWNER_MISMATCH ||
        frame_allocator_process_page_certificate_get(&after) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&before, &after) ||
        frame_release(generic) != FRAME_STATUS_OK ||
        frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) !=
            FRAME_STATUS_OK ||
        frame_allocator_process_page_certificate_get(&allocated) !=
            FRAME_STATUS_OK ||
        frame_release_owned(address, FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_OK ||
        frame_allocator_process_page_certificate_get(&released) !=
            FRAME_STATUS_OK ||
        frame_release_owned(address, FRAME_OWNER_PROCESS_PAGE) !=
            FRAME_STATUS_DOUBLE_FREE ||
        frame_allocator_process_page_certificate_get(&after) !=
            FRAME_STATUS_OK ||
        !process_certificates_are_equal(&released, &after) ||
        allocated.mutation_epoch != before.mutation_epoch + 1U ||
        released.mutation_epoch != allocated.mutation_epoch + 1U ||
        released.owned_frames != 0U) {
        return false;
    }

    return frame_allocator_validate() == FRAME_STATUS_OK;
}

static bool run_process_reinitialize_scenario(void)
{
    const struct boot_context context = test_context();
    uintptr_t address = 0U;
    enum frame_owner owner = FRAME_OWNER_PROCESS_PAGE;
    struct frame_allocator_process_page_certificate allocated = {0U};
    struct frame_allocator_process_page_certificate reinitialized = {0U};
    struct frame_allocator_process_page_certificate proof = {0U};

    return frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) ==
            FRAME_STATUS_OK &&
        frame_allocator_process_page_certificate_get(&allocated) ==
            FRAME_STATUS_OK &&
        allocated.owned_frames == 1U &&
        frame_allocator_initialize(&context) == FRAME_STATUS_OK &&
        frame_allocator_process_page_certificate_get(&reinitialized) ==
            FRAME_STATUS_OK &&
        reinitialized.mutation_epoch == allocated.mutation_epoch + 1U &&
        reinitialized.owned_frames == 0U &&
        frame_query_owner(address, &owner) == FRAME_STATUS_OK &&
        owner == FRAME_OWNER_NONE &&
        process_owner_proof_fails(
            &address, 1U, NULL, 0U, FRAME_STATUS_OWNER_MISMATCH
        ) &&
        frame_allocator_validate_process_page_owners(
            NULL,
            0U,
            NULL,
            0U,
            &proof
        ) == FRAME_STATUS_OK &&
        process_certificates_are_equal(&reinitialized, &proof) &&
        frame_allocator_validate() == FRAME_STATUS_OK;
}

static bool run_scenario(const char *scenario)
{
    uintptr_t address = 0U;
    const struct boot_context context = test_context();

    if (strcmp(scenario, "pre-init") == 0) {
        struct frame_allocator_task_stack_certificate certificate = {
            UINT64_MAX, SIZE_MAX
        };
        struct frame_allocator_process_page_certificate process_certificate = {
            UINT64_MAX, SIZE_MAX
        };

        address = UINTPTR_MAX;
        return frame_allocator_validate() == FRAME_STATUS_NOT_INITIALIZED &&
            frame_allocate(&address) == FRAME_STATUS_NOT_INITIALIZED &&
            address == 0U && stats_are_zero(frame_allocator_get_stats()) &&
            frame_allocator_task_stack_certificate_get(&certificate) ==
                FRAME_STATUS_NOT_INITIALIZED &&
            certificate_is_zero(&certificate) &&
            frame_allocator_process_page_certificate_get(
                &process_certificate
            ) == FRAME_STATUS_NOT_INITIALIZED &&
            process_certificate_is_zero(&process_certificate) &&
            frame_allocator_validate_process_page_owners(
                NULL,
                0U,
                NULL,
                0U,
                &process_certificate
            ) == FRAME_STATUS_NOT_INITIALIZED &&
            process_certificate_is_zero(&process_certificate);
    }

    if (strcmp(scenario, "initialize") == 0) {
        fail_next_lock_release = true;
        return frame_allocator_initialize(&context) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            poisoned_surface_is_fail_closed();
    }

    if (!initialize_allocator()) {
        return false;
    }

    if (strcmp(scenario, "certificate") == 0) {
        struct frame_allocator_task_stack_certificate before = {0U};
        struct frame_allocator_task_stack_certificate allocated = {0U};
        struct frame_allocator_task_stack_certificate released = {0U};

        interrupts_enabled = true;
        if (frame_allocator_task_stack_certificate_get(&before) !=
                FRAME_STATUS_OK ||
            !interrupts_enabled || before.mutation_epoch == 0U ||
            before.owned_frames != 0U) {
            return false;
        }
        interrupts_enabled = false;
        return frame_allocate_owned(FRAME_OWNER_TASK_STACK, &address) ==
                FRAME_STATUS_OK &&
            frame_allocator_task_stack_certificate_get(&allocated) ==
                FRAME_STATUS_OK &&
            allocated.mutation_epoch == before.mutation_epoch + 1U &&
            allocated.owned_frames == 1U &&
            frame_release_owned(address, FRAME_OWNER_TASK_STACK) ==
                FRAME_STATUS_OK &&
            frame_allocator_task_stack_certificate_get(&released) ==
                FRAME_STATUS_OK &&
            released.mutation_epoch == allocated.mutation_epoch + 1U &&
            released.owned_frames == 0U;
    }
    if (strcmp(scenario, "process-certificate") == 0) {
        return run_process_certificate_scenario();
    }
    if (strcmp(scenario, "process-owner-set") == 0) {
        return run_process_owner_set_scenario();
    }
    if (strcmp(scenario, "process-failure-atomicity") == 0) {
        return run_process_failure_atomicity_scenario();
    }
    if (strcmp(scenario, "process-reinitialize") == 0) {
        return run_process_reinitialize_scenario();
    }
    if (strcmp(scenario, "process-epoch-allocate-overflow") == 0) {
        address = UINTPTR_MAX;
        return frame_test_set_process_page_mutation_epoch(
                UINT64_MAX - 1U
            ) &&
            frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            address == 0U && frame_test_process_page_state_matches(0U) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "process-epoch-lifetime") == 0) {
        struct frame_allocator_process_page_certificate certificate = {0U};

        return frame_test_set_process_page_mutation_epoch(
                UINT64_MAX - 2U
            ) &&
            frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) ==
                FRAME_STATUS_OK &&
            frame_release_owned(address, FRAME_OWNER_PROCESS_PAGE) ==
                FRAME_STATUS_OK &&
            frame_allocator_process_page_certificate_get(&certificate) ==
                FRAME_STATUS_OK &&
            certificate.mutation_epoch == UINT64_MAX &&
            certificate.owned_frames == 0U;
    }
    if (strcmp(scenario, "process-epoch-release") == 0) {
        struct frame_allocator_process_page_certificate certificate = {0U};

        return frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) ==
                FRAME_STATUS_OK &&
            frame_test_set_process_page_mutation_epoch(UINT64_MAX - 1U) &&
            frame_release_owned(address, FRAME_OWNER_PROCESS_PAGE) ==
                FRAME_STATUS_OK &&
            frame_allocator_process_page_certificate_get(&certificate) ==
                FRAME_STATUS_OK &&
            certificate.mutation_epoch == UINT64_MAX &&
            certificate.owned_frames == 0U;
    }
    if (strcmp(scenario, "epoch-allocate-overflow") == 0) {
        address = UINTPTR_MAX;
        return frame_test_set_task_stack_mutation_epoch(UINT64_MAX - 1U) &&
            frame_allocate_owned(FRAME_OWNER_TASK_STACK, &address) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            address == 0U && frame_test_task_stack_state_matches(0U) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "epoch-lifetime") == 0) {
        struct frame_allocator_task_stack_certificate certificate = {0U};

        return frame_test_set_task_stack_mutation_epoch(UINT64_MAX - 2U) &&
            frame_allocate_owned(FRAME_OWNER_TASK_STACK, &address) ==
                FRAME_STATUS_OK &&
            frame_release_owned(address, FRAME_OWNER_TASK_STACK) ==
                FRAME_STATUS_OK &&
            frame_allocator_task_stack_certificate_get(&certificate) ==
                FRAME_STATUS_OK &&
            certificate.mutation_epoch == UINT64_MAX &&
            certificate.owned_frames == 0U;
    }
    if (strcmp(scenario, "epoch-release") == 0) {
        struct frame_allocator_task_stack_certificate certificate = {0U};

        return frame_allocate_owned(FRAME_OWNER_TASK_STACK, &address) ==
                FRAME_STATUS_OK &&
            frame_test_set_task_stack_mutation_epoch(UINT64_MAX - 1U) &&
            frame_release_owned(address, FRAME_OWNER_TASK_STACK) ==
                FRAME_STATUS_OK &&
            frame_allocator_task_stack_certificate_get(&certificate) ==
                FRAME_STATUS_OK &&
            certificate.mutation_epoch == UINT64_MAX &&
            certificate.owned_frames == 0U;
    }
    if (strcmp(scenario, "heap-batch") == 0) {
        const struct frame_allocator_stats before =
            frame_allocator_get_stats();
        uintptr_t committed[2] = {0U, 0U};
        uintptr_t staged[1] = {0U};
        uintptr_t duplicate[2];
        uintptr_t generic = 0U;
        uintptr_t probe = 0U;
        struct frame_allocator_stats after;

        if (frame_allocate(&probe) != FRAME_STATUS_OK ||
            frame_release(probe) != FRAME_STATUS_OK ||
            frame_allocator_validate() != FRAME_STATUS_OK) {
            return false;
        }
        after = frame_allocator_get_stats();
        if (!stats_are_equal(&before, &after) ||
            frame_allocate_owned(
                FRAME_OWNER_KERNEL_HEAP,
                &committed[0]
            ) != FRAME_STATUS_OK ||
            frame_allocate_owned(
                FRAME_OWNER_KERNEL_HEAP,
                &committed[1]
            ) != FRAME_STATUS_OK ||
            frame_allocate_owned(FRAME_OWNER_KERNEL_HEAP, &staged[0]) !=
                FRAME_STATUS_OK ||
            frame_allocator_validate_heap_owners(
                committed,
                2U,
                staged,
                1U
            ) != FRAME_STATUS_OK) {
            return false;
        }

        duplicate[0] = committed[0];
        duplicate[1] = committed[0];
        if (frame_allocator_validate_heap_owners(
                duplicate,
                2U,
                staged,
                1U
            ) != FRAME_STATUS_OWNER_MISMATCH ||
            frame_allocator_validate_heap_owners(
                committed,
                2U,
                staged,
                1U
            ) != FRAME_STATUS_OK ||
            frame_allocate(&generic) != FRAME_STATUS_OK) {
            return false;
        }

        duplicate[0] = committed[0];
        duplicate[1] = generic;
        return frame_allocator_validate_heap_owners(
                duplicate,
                2U,
                staged,
                1U
            ) == FRAME_STATUS_OWNER_MISMATCH &&
            frame_allocator_validate_heap_owners(
                committed,
                2U,
                staged,
                1U
            ) == FRAME_STATUS_OK;
    }

    fail_next_lock_release = true;
    if (strcmp(scenario, "process-allocate-lock") == 0) {
        address = UINTPTR_MAX;
        return frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            address == 0U && poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "process-certificate-lock") == 0) {
        struct frame_allocator_process_page_certificate certificate = {
            UINT64_MAX, SIZE_MAX
        };

        return frame_allocator_process_page_certificate_get(&certificate) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            process_certificate_is_zero(&certificate) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "process-owner-set-lock") == 0) {
        struct frame_allocator_process_page_certificate certificate = {
            UINT64_MAX, SIZE_MAX
        };

        fail_next_lock_release = false;
        if (frame_allocate_owned(FRAME_OWNER_PROCESS_PAGE, &address) !=
                FRAME_STATUS_OK) {
            return false;
        }
        fail_next_lock_release = true;
        return frame_allocator_validate_process_page_owners(
                &address,
                1U,
                NULL,
                0U,
                &certificate
            ) == FRAME_STATUS_VALIDATION_FAILURE &&
            process_certificate_is_zero(&certificate) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "allocate") == 0) {
        return frame_allocate(&address) == FRAME_STATUS_VALIDATION_FAILURE &&
            address == 0U && poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "allocate-owned") == 0) {
        return frame_allocate_owned(FRAME_OWNER_KERNEL_HEAP, &address) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            address == 0U && poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "release") == 0) {
        fail_next_lock_release = false;
        if (frame_allocate(&address) != FRAME_STATUS_OK) {
            return false;
        }
        fail_next_lock_release = true;
        return frame_release(address) == FRAME_STATUS_VALIDATION_FAILURE &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "reserve") == 0) {
        return frame_reserve_range(TEST_RESERVE_ADDRESS, ZENITH_PAGE_SIZE) ==
                FRAME_STATUS_VALIDATION_FAILURE &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "inject-failure") == 0) {
        return !frame_test_fail_allocate_after(1U) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "inject-oom") == 0) {
        return !frame_test_report_out_of_memory_after(1U) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "stats") == 0) {
        return stats_are_zero(frame_allocator_get_stats()) &&
            poisoned_surface_is_fail_closed();
    }
    if (strcmp(scenario, "exact-if") == 0) {
        fail_next_lock_release = false;
        interrupts_enabled = true;
        if (frame_allocator_validate() != FRAME_STATUS_OK ||
            !interrupts_enabled) {
            return false;
        }
        interrupts_enabled = false;
        return frame_allocator_validate() == FRAME_STATUS_OK &&
            !interrupts_enabled;
    }
    return false;
}

int main(int argument_count, char **arguments)
{
    if (argument_count != 2 || !run_scenario(arguments[1])) {
        fputs("physical allocator guard-failure scenario failed\n", stderr);
        return 1;
    }

    puts("physical allocator guard-failure scenario passed");
    return 0;
}

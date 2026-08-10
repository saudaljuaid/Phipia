/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/cpu.h>
#include <zenith/memory.h>
#include <zenith/test.h>
#include <zenith/virtual_memory.h>

#define VM_TABLE_ENTRIES 512U
#define VM_TABLE_PAGE_CAPACITY 64U
#define VM_TEST_TABLE_PAGE_CAPACITY 8U
#define VM_MAX_RANGE_PAGES 16384U
#define VM_VGA_ADDRESS UINT64_C(0x00000000000B8000)
#define VM_UNMAPPED_PROBE UINT64_C(0x0000000100000000)

#define VM_ENTRY_PRESENT (UINT64_C(1) << 0U)
#define VM_ENTRY_WRITABLE (UINT64_C(1) << 1U)
#define VM_ENTRY_WRITE_THROUGH (UINT64_C(1) << 3U)
#define VM_ENTRY_CACHE_DISABLE (UINT64_C(1) << 4U)
#define VM_ENTRY_ACCESSED (UINT64_C(1) << 5U)
#define VM_ENTRY_DIRTY (UINT64_C(1) << 6U)
#define VM_ENTRY_HUGE (UINT64_C(1) << 7U)
#define VM_ENTRY_NO_EXECUTE (UINT64_C(1) << 63U)
#define VM_ENTRY_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

#define VM_CR0_WRITE_PROTECT (UINT64_C(1) << 16U)
#define VM_CR0_NOT_WRITE_THROUGH (UINT64_C(1) << 29U)
#define VM_CR0_CACHE_DISABLE (UINT64_C(1) << 30U)
#define VM_EFER_MSR UINT32_C(0xC0000080)
#define VM_EFER_NXE (UINT64_C(1) << 11U)
#define VM_PAT_MSR UINT32_C(0x00000277)
#define VM_PAT_ARCHITECTURAL_DEFAULT UINT64_C(0x0007040600070406)

#define VM_CPUID_FEATURES UINT32_C(1)
#define VM_CPUID_MSR (UINT32_C(1) << 5U)
#define VM_CPUID_PAT (UINT32_C(1) << 16U)
#define VM_CPUID_EXTENDED_MAX UINT32_C(0x80000000)
#define VM_CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define VM_CPUID_EXTENDED_ADDRESS_SIZE UINT32_C(0x80000008)
#define VM_CPUID_NX (UINT32_C(1) << 20U)

#define VM_HEAP_PAGE_COUNT \
    ((size_t)(VIRTUAL_MEMORY_HEAP_SIZE / ZENITH_PAGE_SIZE))
#define VM_TASK_STACK_PAGE_COUNT \
    (VIRTUAL_MEMORY_TASK_STACK_LIMIT * \
        VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES)

struct vm_table_arena {
    uint64_t (*pages)[VM_TABLE_ENTRIES];
    size_t capacity;
    size_t used;
    uint64_t physical_mask;
    uint64_t root_address;
};

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __data_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];

static uint64_t permanent_table_pages[VM_TABLE_PAGE_CAPACITY][VM_TABLE_ENTRIES]
    __attribute__((aligned(ZENITH_PAGE_SIZE)));
static uint64_t test_table_pages[VM_TEST_TABLE_PAGE_CAPACITY][VM_TABLE_ENTRIES]
    __attribute__((aligned(ZENITH_PAGE_SIZE)));
static struct acpi_topology test_device_topology;
static struct virtual_memory_layout test_device_layout;
static struct vm_table_arena permanent_arena;
static struct virtual_memory_layout active_layout;
static uint64_t active_local_apic_physical;
static uint64_t active_io_apic_physical[ACPI_MAX_IO_APICS];
static size_t active_heap_mapped_pages;
static size_t active_task_stack_mapped_pages;
static size_t test_heap_map_successes_remaining;
static bool test_heap_map_failure_enabled;
static bool test_heap_map_uncertain_failure_enabled;
static size_t test_task_stack_map_successes_remaining;
static bool test_task_stack_map_failure_enabled;
static bool test_task_stack_map_uncertain_failure_enabled;
static bool virtual_memory_initialized;

_Static_assert(sizeof(permanent_table_pages[0]) == ZENITH_PAGE_SIZE,
               "one page-table row must occupy one page");
_Static_assert(
    VIRTUAL_MEMORY_DEVICE_WINDOW_BASE % ZENITH_PAGE_SIZE == 0U,
    "device window must be page aligned"
);
_Static_assert(
    VIRTUAL_MEMORY_HEAP_BASE % ZENITH_PAGE_SIZE == 0U &&
        VIRTUAL_MEMORY_HEAP_SIZE % ZENITH_PAGE_SIZE == 0U,
    "heap window must contain whole aligned pages"
);
_Static_assert(
    VIRTUAL_MEMORY_HEAP_SIZE != 0U,
    "heap window must not be empty"
);
_Static_assert(
    VIRTUAL_MEMORY_TASK_STACK_BASE % ZENITH_PAGE_SIZE == 0U &&
        VIRTUAL_MEMORY_TASK_STACK_SLOT_SIZE % ZENITH_PAGE_SIZE == 0U &&
        VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE % ZENITH_PAGE_SIZE == 0U,
    "task-stack window must contain whole aligned pages"
);
_Static_assert(
    VIRTUAL_MEMORY_TASK_STACK_LIMIT == 16U &&
        VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES == 16U,
    "scheduler and virtual-memory task-stack limits disagree"
);

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static bool address_is_page_aligned(uint64_t address)
{
    return (address & (ZENITH_PAGE_SIZE - 1U)) == 0U;
}

static bool address_is_canonical(uint64_t address)
{
    const uint64_t upper = address >> 48U;

    if ((address & (UINT64_C(1) << 47U)) == 0U) {
        return upper == 0U;
    }

    return upper == UINT64_C(0xFFFF);
}

static bool ranges_overlap(
    uint64_t left_start,
    uint64_t left_end,
    uint64_t right_start,
    uint64_t right_end
)
{
    return left_start < right_end && right_start < left_end;
}

static bool address_is_in_heap_window(uint64_t address)
{
    return address >= VIRTUAL_MEMORY_HEAP_BASE &&
        address - VIRTUAL_MEMORY_HEAP_BASE < VIRTUAL_MEMORY_HEAP_SIZE;
}

static enum virtual_memory_status task_stack_page_address(
    size_t slot_index,
    size_t page_index,
    uint64_t *address
)
{
    uint64_t slot_offset;
    uint64_t page_offset;

    if (address == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    *address = 0U;

    if (slot_index >= VIRTUAL_MEMORY_TASK_STACK_LIMIT) {
        return VIRTUAL_MEMORY_STATUS_BAD_STACK_SLOT;
    }

    if (page_index >= VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES) {
        return VIRTUAL_MEMORY_STATUS_BAD_STACK_PAGE;
    }

    slot_offset = (uint64_t)slot_index *
        VIRTUAL_MEMORY_TASK_STACK_SLOT_SIZE;
    page_offset = ((uint64_t)page_index + 1U) * ZENITH_PAGE_SIZE;

    if (slot_offset > UINT64_MAX - VIRTUAL_MEMORY_TASK_STACK_BASE ||
        page_offset > UINT64_MAX -
            (VIRTUAL_MEMORY_TASK_STACK_BASE + slot_offset)) {
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    *address = VIRTUAL_MEMORY_TASK_STACK_BASE + slot_offset + page_offset;

    if (!address_is_page_aligned(*address) ||
        !address_is_canonical(*address) ||
        *address < VIRTUAL_MEMORY_TASK_STACK_BASE ||
        *address >= VIRTUAL_MEMORY_TASK_STACK_WINDOW_END) {
        *address = 0U;
        return VIRTUAL_MEMORY_STATUS_OUTSIDE_TASK_STACK_WINDOW;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static bool address_fits_mask(uint64_t address, uint64_t physical_mask)
{
    return (address & ~physical_mask) == 0U;
}

static bool permissions_are_valid(uint32_t permissions)
{
    const uint32_t allowed = VIRTUAL_MEMORY_WRITABLE |
        VIRTUAL_MEMORY_EXECUTABLE | VIRTUAL_MEMORY_DEVICE;
    const bool writable =
        (permissions & VIRTUAL_MEMORY_WRITABLE) != 0U;
    const bool executable =
        (permissions & VIRTUAL_MEMORY_EXECUTABLE) != 0U;
    const bool device = (permissions & VIRTUAL_MEMORY_DEVICE) != 0U;

    if ((permissions & ~allowed) != 0U || (writable && executable)) {
        return false;
    }

    return !device || (writable && !executable);
}

static bool table_address_is_owned(
    const struct vm_table_arena *arena,
    uint64_t address
)
{
    const uint64_t base = (uint64_t)(uintptr_t)(void *)arena->pages;
    const uint64_t length = arena->used * ZENITH_PAGE_SIZE;

    return arena->used <= arena->capacity &&
        address_is_page_aligned(address) && address >= base &&
        address - base < length;
}

static bool parent_entry_is_valid(
    const struct vm_table_arena *arena,
    uint64_t entry
)
{
    const uint64_t allowed = VM_ENTRY_ADDRESS_MASK | VM_ENTRY_PRESENT |
        VM_ENTRY_WRITABLE | VM_ENTRY_ACCESSED;
    const uint64_t address = entry & VM_ENTRY_ADDRESS_MASK;

    return (entry & ~allowed) == 0U &&
        (entry & (VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE)) ==
            (VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE) &&
        address_fits_mask(address, arena->physical_mask) &&
        table_address_is_owned(arena, address);
}

static enum virtual_memory_status arena_allocate_table(
    struct vm_table_arena *arena,
    uint64_t **table,
    uint64_t *physical_address
)
{
    uint64_t address;

    if (arena->used == arena->capacity) {
        return VIRTUAL_MEMORY_STATUS_TABLE_LIMIT;
    }

    *table = arena->pages[arena->used];
    address = (uint64_t)(uintptr_t)(void *)*table;

    if (!address_is_page_aligned(address) ||
        !address_fits_mask(address, arena->physical_mask)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    bytes_zero(*table, ZENITH_PAGE_SIZE);
    ++arena->used;
    *physical_address = address;
    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status arena_initialize(
    struct vm_table_arena *arena,
    uint64_t (*pages)[VM_TABLE_ENTRIES],
    size_t capacity,
    uint64_t physical_mask
)
{
    uint64_t *root;

    if (arena == NULL || pages == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (capacity == 0U || capacity > VM_TABLE_PAGE_CAPACITY) {
        return VIRTUAL_MEMORY_STATUS_TABLE_LIMIT;
    }

    arena->pages = pages;
    arena->capacity = capacity;
    arena->used = 0U;
    arena->physical_mask = physical_mask;
    arena->root_address = 0U;
    return arena_allocate_table(arena, &root, &arena->root_address);
}

static enum virtual_memory_status descend_or_allocate(
    struct vm_table_arena *arena,
    uint64_t *table,
    size_t index,
    uint64_t **next_table
)
{
    uint64_t entry = table[index];
    uint64_t address;
    enum virtual_memory_status status;

    if ((entry & VM_ENTRY_PRESENT) == 0U) {
        if (entry != 0U) {
            return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        status = arena_allocate_table(arena, next_table, &address);

        if (status != VIRTUAL_MEMORY_STATUS_OK) {
            return status;
        }

        table[index] = address | VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE;
        return VIRTUAL_MEMORY_STATUS_OK;
    }

    address = entry & VM_ENTRY_ADDRESS_MASK;

    if ((entry & VM_ENTRY_HUGE) != 0U) {
        return VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT;
    }

    if (!parent_entry_is_valid(arena, entry)) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    *next_table = (uint64_t *)(uintptr_t)address;
    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status locate_leaf_entry(
    const struct vm_table_arena *arena,
    uint64_t virtual_address,
    uint64_t **leaf_entry
)
{
    const size_t indices[4] = {
        (size_t)((virtual_address >> 39U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 30U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 21U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 12U) & UINT64_C(0x1FF))
    };
    uint64_t *table;

    if (arena == NULL || leaf_entry == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (!address_is_canonical(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    table = arena->pages[0];

    for (size_t level = 0U; level < 3U; ++level) {
        const uint64_t entry = table[indices[level]];
        const uint64_t address = entry & VM_ENTRY_ADDRESS_MASK;

        if ((entry & VM_ENTRY_PRESENT) == 0U) {
            return entry == 0U
                ? VIRTUAL_MEMORY_STATUS_NOT_MAPPED
                : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        if ((entry & VM_ENTRY_HUGE) != 0U) {
            return VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT;
        }

        if (!parent_entry_is_valid(arena, entry)) {
            return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        table = (uint64_t *)(uintptr_t)address;
    }

    *leaf_entry = &table[indices[3]];
    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status prepare_leaf_table(
    struct vm_table_arena *arena,
    uint64_t virtual_address
)
{
    const size_t pml4_index =
        (size_t)((virtual_address >> 39U) & UINT64_C(0x1FF));
    const size_t pdpt_index =
        (size_t)((virtual_address >> 30U) & UINT64_C(0x1FF));
    const size_t directory_index =
        (size_t)((virtual_address >> 21U) & UINT64_C(0x1FF));
    uint64_t *pdpt;
    uint64_t *directory;
    uint64_t *page_table;
    enum virtual_memory_status status;

    status = descend_or_allocate(
        arena,
        arena->pages[0],
        pml4_index,
        &pdpt
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = descend_or_allocate(arena, pdpt, pdpt_index, &directory);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    return descend_or_allocate(
        arena,
        directory,
        directory_index,
        &page_table
    );
}

static enum virtual_memory_status prepare_heap_window(
    struct vm_table_arena *arena
)
{
    for (size_t page = 0U; page < VM_HEAP_PAGE_COUNT; ++page) {
        const uint64_t address = VIRTUAL_MEMORY_HEAP_BASE +
            (uint64_t)page * ZENITH_PAGE_SIZE;
        enum virtual_memory_status status = prepare_leaf_table(arena, address);

        if (status != VIRTUAL_MEMORY_STATUS_OK) {
            return status;
        }
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status prepare_task_stack_window(
    struct vm_table_arena *arena
)
{
    for (size_t slot = 0U;
         slot < VIRTUAL_MEMORY_TASK_STACK_LIMIT;
         ++slot) {
        for (size_t page = 0U;
             page < VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES;
             ++page) {
            uint64_t address;
            enum virtual_memory_status status = task_stack_page_address(
                slot,
                page,
                &address
            );

            if (status != VIRTUAL_MEMORY_STATUS_OK) {
                return status;
            }

            status = prepare_leaf_table(arena, address);

            if (status != VIRTUAL_MEMORY_STATUS_OK) {
                return status;
            }
        }
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status map_page(
    struct vm_table_arena *arena,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
)
{
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *directory;
    uint64_t *page_table;
    uint64_t entry;
    size_t pml4_index;
    size_t pdpt_index;
    size_t directory_index;
    size_t page_index;
    enum virtual_memory_status status;

    if (arena == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (!address_is_page_aligned(virtual_address) ||
        !address_is_page_aligned(physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (!address_is_canonical(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    if (!address_fits_mask(physical_address, arena->physical_mask)) {
        return VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE;
    }

    if (!permissions_are_valid(permissions)) {
        return VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS;
    }

    pml4_index = (size_t)((virtual_address >> 39U) & UINT64_C(0x1FF));
    pdpt_index = (size_t)((virtual_address >> 30U) & UINT64_C(0x1FF));
    directory_index = (size_t)((virtual_address >> 21U) & UINT64_C(0x1FF));
    page_index = (size_t)((virtual_address >> 12U) & UINT64_C(0x1FF));
    pml4 = arena->pages[0];

    status = descend_or_allocate(arena, pml4, pml4_index, &pdpt);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = descend_or_allocate(arena, pdpt, pdpt_index, &directory);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = descend_or_allocate(
        arena,
        directory,
        directory_index,
        &page_table
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if (page_table[page_index] != 0U) {
        return (page_table[page_index] & VM_ENTRY_PRESENT) != 0U
            ? VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT
            : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    entry = physical_address | VM_ENTRY_PRESENT;

    if ((permissions & VIRTUAL_MEMORY_WRITABLE) != 0U) {
        entry |= VM_ENTRY_WRITABLE;
    }

    if ((permissions & VIRTUAL_MEMORY_EXECUTABLE) == 0U) {
        entry |= VM_ENTRY_NO_EXECUTE;
    }

    if ((permissions & VIRTUAL_MEMORY_DEVICE) != 0U) {
        entry |= VM_ENTRY_WRITE_THROUGH | VM_ENTRY_CACHE_DISABLE;
    }

    page_table[page_index] = entry;
    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status map_range(
    struct vm_table_arena *arena,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t length,
    uint32_t permissions
)
{
    uint64_t page_count;

    if (length == 0U || !address_is_page_aligned(length) ||
        !address_is_page_aligned(virtual_address) ||
        !address_is_page_aligned(physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (length - 1U > UINT64_MAX - virtual_address ||
        length - 1U > UINT64_MAX - physical_address) {
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    if (!address_is_canonical(virtual_address + length - 1U)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    page_count = length / ZENITH_PAGE_SIZE;

    if (page_count > VM_MAX_RANGE_PAGES) {
        return VIRTUAL_MEMORY_STATUS_RANGE_TOO_LARGE;
    }

    for (uint64_t page = 0U; page < page_count; ++page) {
        const uint64_t offset = page * ZENITH_PAGE_SIZE;
        enum virtual_memory_status status = map_page(
            arena,
            virtual_address + offset,
            physical_address + offset,
            permissions
        );

        if (status != VIRTUAL_MEMORY_STATUS_OK) {
            return status;
        }
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status query_arena(
    const struct vm_table_arena *arena,
    uint64_t virtual_address,
    struct virtual_memory_mapping *mapping
)
{
    const size_t indices[4] = {
        (size_t)((virtual_address >> 39U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 30U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 21U) & UINT64_C(0x1FF)),
        (size_t)((virtual_address >> 12U) & UINT64_C(0x1FF))
    };
    const uint64_t *table;
    uint64_t entry;

    if (arena == NULL || mapping == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (!address_is_canonical(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    table = arena->pages[0];

    for (size_t level = 0U; level < 3U; ++level) {
        uint64_t address;

        entry = table[indices[level]];

        if ((entry & VM_ENTRY_PRESENT) == 0U) {
            return entry == 0U
                ? VIRTUAL_MEMORY_STATUS_NOT_MAPPED
                : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        address = entry & VM_ENTRY_ADDRESS_MASK;

        if (!parent_entry_is_valid(arena, entry)) {
            return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        table = (const uint64_t *)(uintptr_t)address;
    }

    entry = table[indices[3]];

    if ((entry & VM_ENTRY_PRESENT) == 0U) {
        return entry == 0U
            ? VIRTUAL_MEMORY_STATUS_NOT_MAPPED
            : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if ((entry & ~(VM_ENTRY_ADDRESS_MASK | VM_ENTRY_PRESENT |
            VM_ENTRY_WRITABLE | VM_ENTRY_WRITE_THROUGH |
            VM_ENTRY_CACHE_DISABLE | VM_ENTRY_ACCESSED | VM_ENTRY_DIRTY |
            VM_ENTRY_NO_EXECUTE)) != 0U ||
        !address_fits_mask(
            entry & VM_ENTRY_ADDRESS_MASK,
            arena->physical_mask
        )) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    mapping->physical_address = (entry & VM_ENTRY_ADDRESS_MASK) |
        (virtual_address & (ZENITH_PAGE_SIZE - 1U));
    mapping->permissions = 0U;

    if ((entry & VM_ENTRY_WRITABLE) != 0U) {
        mapping->permissions |= VIRTUAL_MEMORY_WRITABLE;
    }

    if ((entry & VM_ENTRY_NO_EXECUTE) == 0U) {
        mapping->permissions |= VIRTUAL_MEMORY_EXECUTABLE;
    }

    if ((entry & (VM_ENTRY_WRITE_THROUGH | VM_ENTRY_CACHE_DISABLE)) != 0U) {
        if ((entry & (VM_ENTRY_WRITE_THROUGH | VM_ENTRY_CACHE_DISABLE)) !=
            (VM_ENTRY_WRITE_THROUGH | VM_ENTRY_CACHE_DISABLE)) {
            return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
        }

        mapping->permissions |= VIRTUAL_MEMORY_DEVICE;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static uint64_t linker_address(const uint8_t *symbol)
{
    return (uint64_t)(uintptr_t)(const void *)symbol;
}

static bool heap_window_layout_is_valid(void)
{
    const uint64_t kernel_start = linker_address(__kernel_start);
    const uint64_t kernel_end = linker_address(__kernel_end);
    const uint64_t heap_reserved_start = VIRTUAL_MEMORY_HEAP_GUARD_BELOW;
    uint64_t heap_payload_end;
    uint64_t heap_reserved_end;
    uint64_t device_end;

    if (VIRTUAL_MEMORY_HEAP_SIZE >
            UINT64_MAX - VIRTUAL_MEMORY_HEAP_BASE ||
        VIRTUAL_MEMORY_HEAP_GUARD_ABOVE > UINT64_MAX - ZENITH_PAGE_SIZE ||
        VIRTUAL_MEMORY_HEAP_GUARD_BELOW > UINT64_MAX - ZENITH_PAGE_SIZE ||
        (ACPI_MAX_IO_APICS + 1U) >
            (UINT64_MAX - VIRTUAL_MEMORY_DEVICE_WINDOW_BASE) /
                ZENITH_PAGE_SIZE) {
        return false;
    }

    heap_payload_end = VIRTUAL_MEMORY_HEAP_BASE + VIRTUAL_MEMORY_HEAP_SIZE;
    heap_reserved_end = VIRTUAL_MEMORY_HEAP_GUARD_ABOVE + ZENITH_PAGE_SIZE;
    device_end = VIRTUAL_MEMORY_DEVICE_WINDOW_BASE +
        (ACPI_MAX_IO_APICS + 1U) * ZENITH_PAGE_SIZE;

    return address_is_page_aligned(heap_reserved_start) &&
        address_is_page_aligned(heap_reserved_end) &&
        address_is_canonical(heap_reserved_start) &&
        address_is_canonical(heap_reserved_end - 1U) &&
        heap_reserved_start + ZENITH_PAGE_SIZE ==
            VIRTUAL_MEMORY_HEAP_BASE &&
        heap_payload_end == VIRTUAL_MEMORY_HEAP_GUARD_ABOVE &&
        VIRTUAL_MEMORY_HEAP_GUARD_ABOVE < heap_reserved_end &&
        !ranges_overlap(
            heap_reserved_start,
            heap_reserved_end,
            kernel_start,
            kernel_end
        ) &&
        !ranges_overlap(
            heap_reserved_start,
            heap_reserved_end,
            VM_VGA_ADDRESS,
            VM_VGA_ADDRESS + ZENITH_PAGE_SIZE
        ) &&
        !ranges_overlap(
            heap_reserved_start,
            heap_reserved_end,
            VM_UNMAPPED_PROBE,
            VM_UNMAPPED_PROBE + ZENITH_PAGE_SIZE
        ) &&
        !ranges_overlap(
            heap_reserved_start,
            heap_reserved_end,
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE,
            device_end
        );
}

static bool task_stack_window_layout_is_valid(void)
{
    const uint64_t kernel_start = linker_address(__kernel_start);
    const uint64_t kernel_end = linker_address(__kernel_end);
    const uint64_t heap_reserved_start = VIRTUAL_MEMORY_HEAP_GUARD_BELOW;
    const uint64_t heap_reserved_end = VIRTUAL_MEMORY_HEAP_GUARD_ABOVE +
        ZENITH_PAGE_SIZE;
    uint64_t device_end;

    if (VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE == 0U ||
        VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE >
            UINT64_MAX - VIRTUAL_MEMORY_TASK_STACK_BASE ||
        VIRTUAL_MEMORY_TASK_STACK_WINDOW_END !=
            VIRTUAL_MEMORY_TASK_STACK_BASE +
                VIRTUAL_MEMORY_TASK_STACK_WINDOW_SIZE ||
        (ACPI_MAX_IO_APICS + 1U) >
            (UINT64_MAX - VIRTUAL_MEMORY_DEVICE_WINDOW_BASE) /
                ZENITH_PAGE_SIZE) {
        return false;
    }

    device_end = VIRTUAL_MEMORY_DEVICE_WINDOW_BASE +
        (ACPI_MAX_IO_APICS + 1U) * ZENITH_PAGE_SIZE;

    if (!address_is_page_aligned(VIRTUAL_MEMORY_TASK_STACK_BASE) ||
        !address_is_page_aligned(VIRTUAL_MEMORY_TASK_STACK_WINDOW_END) ||
        !address_is_canonical(VIRTUAL_MEMORY_TASK_STACK_BASE) ||
        !address_is_canonical(VIRTUAL_MEMORY_TASK_STACK_WINDOW_END - 1U) ||
        ranges_overlap(
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END,
            kernel_start,
            kernel_end
        ) ||
        ranges_overlap(
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END,
            VM_VGA_ADDRESS,
            VM_VGA_ADDRESS + ZENITH_PAGE_SIZE
        ) ||
        ranges_overlap(
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END,
            VM_UNMAPPED_PROBE,
            VM_UNMAPPED_PROBE + ZENITH_PAGE_SIZE
        ) ||
        ranges_overlap(
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END,
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE,
            device_end
        ) ||
        ranges_overlap(
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END,
            heap_reserved_start,
            heap_reserved_end
        )) {
        return false;
    }

    for (size_t slot = 0U;
         slot < VIRTUAL_MEMORY_TASK_STACK_LIMIT;
         ++slot) {
        uint64_t lower;
        uint64_t payload_start;
        uint64_t payload_end;
        uint64_t upper;

        if (virtual_memory_task_stack_bounds(
                slot,
                &lower,
                &payload_start,
                &payload_end,
                &upper
            ) != VIRTUAL_MEMORY_STATUS_OK ||
            lower + ZENITH_PAGE_SIZE != payload_start ||
            payload_end != upper ||
            upper + ZENITH_PAGE_SIZE >
                VIRTUAL_MEMORY_TASK_STACK_WINDOW_END ||
            payload_end - payload_start !=
                VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES * ZENITH_PAGE_SIZE) {
            return false;
        }
    }

    return true;
}

static bool kernel_layout_is_valid(void)
{
    const uint64_t kernel_start = linker_address(__kernel_start);
    const uint64_t kernel_end = linker_address(__kernel_end);
    const uint64_t text_start = linker_address(__text_start);
    const uint64_t text_end = linker_address(__text_end);
    const uint64_t rodata_start = linker_address(__rodata_start);
    const uint64_t rodata_end = linker_address(__rodata_end);
    const uint64_t data_start = linker_address(__data_start);
    const uint64_t data_end = linker_address(__data_end);
    const uint64_t bss_start = linker_address(__bss_start);
    const uint64_t bss_end = linker_address(__bss_end);

    return heap_window_layout_is_valid() &&
        task_stack_window_layout_is_valid() &&
        address_is_page_aligned(kernel_start) &&
        address_is_page_aligned(kernel_end) &&
        address_is_page_aligned(text_start) &&
        address_is_page_aligned(text_end) &&
        address_is_page_aligned(rodata_start) &&
        address_is_page_aligned(rodata_end) &&
        address_is_page_aligned(data_start) &&
        address_is_page_aligned(data_end) &&
        address_is_page_aligned(bss_start) &&
        address_is_page_aligned(bss_end) &&
        kernel_start < text_start &&
        text_start < text_end && text_end == rodata_start &&
        rodata_start < rodata_end && rodata_end == data_start &&
        data_start < data_end && data_end == bss_start &&
        bss_start < bss_end && bss_end == kernel_end &&
        kernel_end - kernel_start <=
            VM_MAX_RANGE_PAGES * ZENITH_PAGE_SIZE;
}

static enum virtual_memory_status physical_mask_detect(uint64_t *mask)
{
    struct cpu_cpuid_result result;
    uint32_t extended_max;
    uint32_t physical_bits = 36U;

    if (mask == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    cpu_cpuid(VM_CPUID_FEATURES, 0U, &result);

    if ((result.edx & (VM_CPUID_MSR | VM_CPUID_PAT)) !=
        (VM_CPUID_MSR | VM_CPUID_PAT)) {
        return VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU;
    }

    cpu_cpuid(VM_CPUID_EXTENDED_MAX, 0U, &result);
    extended_max = result.eax;

    if (extended_max < VM_CPUID_EXTENDED_FEATURES) {
        return VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU;
    }

    cpu_cpuid(VM_CPUID_EXTENDED_FEATURES, 0U, &result);

    if ((result.edx & VM_CPUID_NX) == 0U) {
        return VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU;
    }

    if (extended_max >= VM_CPUID_EXTENDED_ADDRESS_SIZE) {
        cpu_cpuid(VM_CPUID_EXTENDED_ADDRESS_SIZE, 0U, &result);
        physical_bits = result.eax & UINT32_C(0xFF);
    }

    if (physical_bits < 32U || physical_bits > 52U) {
        return VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU;
    }

    *mask = ((UINT64_C(1) << physical_bits) - 1U) &
        VM_ENTRY_ADDRESS_MASK;
    return VIRTUAL_MEMORY_STATUS_OK;
}

static void pat_write_serialized(uint64_t value)
{
    const uint64_t original_cr0 = cpu_read_cr0();
    const uint64_t cache_disabled =
        (original_cr0 | VM_CR0_CACHE_DISABLE) & ~VM_CR0_NOT_WRITE_THROUGH;

    cpu_write_cr0(cache_disabled);
    cpu_write_back_and_invalidate();
    cpu_write_msr(VM_PAT_MSR, value);
    cpu_write_back_and_invalidate();
    cpu_write_cr0(original_cr0);
    cpu_write_cr3(cpu_read_cr3());
}

static enum virtual_memory_status pat_configure(
    uint64_t *previous_value,
    bool *changed
)
{
    if (previous_value == NULL || changed == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    *previous_value = cpu_read_msr(VM_PAT_MSR);
    *changed = *previous_value != VM_PAT_ARCHITECTURAL_DEFAULT;

    if (*changed) {
        pat_write_serialized(VM_PAT_ARCHITECTURAL_DEFAULT);
    }

    if (cpu_read_msr(VM_PAT_MSR) != VM_PAT_ARCHITECTURAL_DEFAULT) {
        if (*changed) {
            pat_write_serialized(*previous_value);
        }

        return VIRTUAL_MEMORY_STATUS_PAT_FAILURE;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

static enum virtual_memory_status map_kernel(void)
{
    const uint64_t kernel_start = linker_address(__kernel_start);
    const uint64_t kernel_end = linker_address(__kernel_end);
    const uint64_t text_start = linker_address(__text_start);
    const uint64_t rodata_start = linker_address(__rodata_start);
    const uint64_t data_start = linker_address(__data_start);
    enum virtual_memory_status status;

    status = map_range(
        &permanent_arena,
        kernel_start,
        kernel_start,
        text_start - kernel_start,
        VIRTUAL_MEMORY_READ_ONLY
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = map_range(
        &permanent_arena,
        text_start,
        text_start,
        rodata_start - text_start,
        VIRTUAL_MEMORY_EXECUTABLE
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = map_range(
        &permanent_arena,
        rodata_start,
        rodata_start,
        data_start - rodata_start,
        VIRTUAL_MEMORY_READ_ONLY
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    return map_range(
        &permanent_arena,
        data_start,
        data_start,
        kernel_end - data_start,
        VIRTUAL_MEMORY_WRITABLE
    );
}

static enum virtual_memory_status map_devices(
    struct vm_table_arena *arena,
    const struct acpi_topology *topology,
    struct virtual_memory_layout *layout
)
{
    enum virtual_memory_status status;

    if (topology->io_apic_count > ACPI_MAX_IO_APICS) {
        return VIRTUAL_MEMORY_STATUS_TOO_MANY_DEVICES;
    }

    if (topology->local_apic_address == 0U ||
        !address_is_page_aligned(topology->local_apic_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS;
    }

    layout->local_apic_virtual_address =
        VIRTUAL_MEMORY_DEVICE_WINDOW_BASE;
    status = map_page(
        arena,
        layout->local_apic_virtual_address,
        topology->local_apic_address,
        VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
    );

    if (status == VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT ||
        status == VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE) {
        return VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS;
    }

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        const uint64_t physical_address =
            topology->io_apics[index].physical_address;
        const uint64_t virtual_address =
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE +
            (index + 1U) * ZENITH_PAGE_SIZE;

        if (physical_address == 0U ||
            !address_is_page_aligned(physical_address)) {
            return VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS;
        }

        status = map_page(
            arena,
            virtual_address,
            physical_address,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        );

        if (status == VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT ||
            status == VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE) {
            return VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS;
        }

        if (status != VIRTUAL_MEMORY_STATUS_OK) {
            return status;
        }

        layout->io_apic_virtual_addresses[index] = virtual_address;
    }

    layout->io_apic_count = topology->io_apic_count;
    return VIRTUAL_MEMORY_STATUS_OK;
}

static bool mapping_matches(
    const struct vm_table_arena *arena,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
)
{
    struct virtual_memory_mapping mapping;

    return query_arena(arena, virtual_address, &mapping) ==
            VIRTUAL_MEMORY_STATUS_OK &&
        mapping.physical_address == physical_address &&
        mapping.permissions == permissions;
}

static bool range_matches(
    const struct vm_table_arena *arena,
    uint64_t start,
    uint64_t end,
    uint32_t permissions
)
{
    for (uint64_t address = start; address < end;
         address += ZENITH_PAGE_SIZE) {
        if (!mapping_matches(
                arena,
                address,
                address,
                permissions
            )) {
            return false;
        }
    }

    return true;
}

static bool layout_core_matches(
    const struct vm_table_arena *arena,
    const struct virtual_memory_layout *layout,
    size_t io_apic_count
)
{
    struct virtual_memory_mapping ignored;
    const uint64_t kernel_start = linker_address(__kernel_start);
    const uint64_t kernel_end = linker_address(__kernel_end);
    const uint64_t text_start = linker_address(__text_start);
    const uint64_t rodata_start = linker_address(__rodata_start);
    const uint64_t data_start = linker_address(__data_start);

    if (io_apic_count > ACPI_MAX_IO_APICS ||
        arena->used > arena->capacity ||
        layout->root_table_address != arena->root_address ||
        layout->table_page_count != arena->used ||
        layout->io_apic_count != io_apic_count ||
        layout->local_apic_virtual_address !=
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE ||
        query_arena(arena, 0U, &ignored) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(arena, VM_UNMAPPED_PROBE, &ignored) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(arena, kernel_end, &ignored) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        return false;
    }

    return range_matches(
            arena,
            kernel_start,
            text_start,
            VIRTUAL_MEMORY_READ_ONLY
        ) &&
        range_matches(
            arena,
            text_start,
            rodata_start,
            VIRTUAL_MEMORY_EXECUTABLE
        ) &&
        range_matches(
            arena,
            rodata_start,
            data_start,
            VIRTUAL_MEMORY_READ_ONLY
        ) &&
        range_matches(
            arena,
            data_start,
            kernel_end,
            VIRTUAL_MEMORY_WRITABLE
        ) &&
        mapping_matches(
            arena,
            VM_VGA_ADDRESS,
            VM_VGA_ADDRESS,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        );
}

static bool heap_window_matches(const struct vm_table_arena *arena)
{
    struct virtual_memory_mapping ignored;
    size_t mapped_pages = 0U;
    const uint64_t allowed_leaf_bits = VM_ENTRY_ADDRESS_MASK |
        VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE | VM_ENTRY_ACCESSED |
        VM_ENTRY_DIRTY | VM_ENTRY_NO_EXECUTE;

    if (!heap_window_layout_is_valid() ||
        query_arena(arena, VIRTUAL_MEMORY_HEAP_GUARD_BELOW, &ignored) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(arena, VIRTUAL_MEMORY_HEAP_GUARD_ABOVE, &ignored) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        return false;
    }

    for (size_t page = 0U; page < VM_HEAP_PAGE_COUNT; ++page) {
        const uint64_t address = VIRTUAL_MEMORY_HEAP_BASE +
            (uint64_t)page * ZENITH_PAGE_SIZE;
        uint64_t *leaf_entry;
        uint64_t entry;

        if (locate_leaf_entry(arena, address, &leaf_entry) !=
            VIRTUAL_MEMORY_STATUS_OK) {
            return false;
        }

        entry = *leaf_entry;

        if ((entry & VM_ENTRY_PRESENT) == 0U) {
            if (entry != 0U) {
                return false;
            }

            continue;
        }

        if ((entry & ~allowed_leaf_bits) != 0U ||
            (entry & (VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE)) !=
                (VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE) ||
            !address_fits_mask(
                entry & VM_ENTRY_ADDRESS_MASK,
                arena->physical_mask
            )) {
            return false;
        }

        ++mapped_pages;
    }

    return mapped_pages == active_heap_mapped_pages;
}

static bool task_stack_window_matches(const struct vm_table_arena *arena)
{
    struct virtual_memory_mapping ignored;
    size_t mapped_pages = 0U;
    const uint64_t allowed_leaf_bits = VM_ENTRY_ADDRESS_MASK |
        VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE | VM_ENTRY_ACCESSED |
        VM_ENTRY_DIRTY | VM_ENTRY_NO_EXECUTE;

    if (!task_stack_window_layout_is_valid()) {
        return false;
    }

    for (size_t slot = 0U;
         slot < VIRTUAL_MEMORY_TASK_STACK_LIMIT;
         ++slot) {
        uint64_t lower;
        uint64_t payload_start;
        uint64_t payload_end;
        uint64_t upper;

        if (virtual_memory_task_stack_bounds(
                slot,
                &lower,
                &payload_start,
                &payload_end,
                &upper
            ) != VIRTUAL_MEMORY_STATUS_OK ||
            query_arena(arena, lower, &ignored) !=
                VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
            query_arena(arena, upper, &ignored) !=
                VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
            return false;
        }

        for (size_t page = 0U;
             page < VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES;
             ++page) {
            uint64_t address;
            uint64_t *leaf_entry;
            uint64_t entry;

            if (task_stack_page_address(slot, page, &address) !=
                    VIRTUAL_MEMORY_STATUS_OK ||
                address < payload_start || address >= payload_end ||
                locate_leaf_entry(arena, address, &leaf_entry) !=
                    VIRTUAL_MEMORY_STATUS_OK) {
                return false;
            }

            entry = *leaf_entry;

            if ((entry & VM_ENTRY_PRESENT) == 0U) {
                if (entry != 0U) {
                    return false;
                }

                continue;
            }

            if ((entry & ~allowed_leaf_bits) != 0U ||
                (entry & (VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE)) !=
                    (VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE) ||
                !address_fits_mask(
                    entry & VM_ENTRY_ADDRESS_MASK,
                    arena->physical_mask
                )) {
                return false;
            }

            ++mapped_pages;
        }
    }

    return mapped_pages == active_task_stack_mapped_pages;
}

static bool layout_matches_topology(
    const struct vm_table_arena *arena,
    const struct acpi_topology *topology,
    const struct virtual_memory_layout *layout
)
{
    if (!layout_core_matches(arena, layout, topology->io_apic_count) ||
        !heap_window_matches(arena) ||
        !task_stack_window_matches(arena) ||
        !mapping_matches(
            arena,
            layout->local_apic_virtual_address,
            topology->local_apic_address,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        )) {
        return false;
    }

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        if (!mapping_matches(
                arena,
                layout->io_apic_virtual_addresses[index],
                topology->io_apics[index].physical_address,
                VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
            )) {
            return false;
        }
    }

    return true;
}

static bool active_layout_matches(void)
{
    if (!layout_core_matches(
            &permanent_arena,
            &active_layout,
            active_layout.io_apic_count
        ) ||
        !heap_window_matches(&permanent_arena) ||
        !task_stack_window_matches(&permanent_arena) ||
        !mapping_matches(
            &permanent_arena,
            active_layout.local_apic_virtual_address,
            active_local_apic_physical,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        )) {
        return false;
    }

    for (size_t index = 0U; index < active_layout.io_apic_count; ++index) {
        if (!mapping_matches(
                &permanent_arena,
                active_layout.io_apic_virtual_addresses[index],
                active_io_apic_physical[index],
                VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
            )) {
            return false;
        }
    }

    return true;
}

static bool permanent_arena_metadata_matches(void)
{
    const uint64_t expected_root =
        (uint64_t)(uintptr_t)(void *)permanent_table_pages[0];

    return virtual_memory_initialized &&
        permanent_arena.pages == permanent_table_pages &&
        permanent_arena.capacity == VM_TABLE_PAGE_CAPACITY &&
        permanent_arena.used != 0U &&
        permanent_arena.used <= permanent_arena.capacity &&
        permanent_arena.root_address == expected_root &&
        active_layout.root_table_address == expected_root &&
        active_layout.table_page_count == permanent_arena.used &&
        permanent_arena.physical_mask != 0U &&
        (permanent_arena.physical_mask & ~VM_ENTRY_ADDRESS_MASK) == 0U &&
        address_fits_mask(expected_root, permanent_arena.physical_mask);
}

static bool runtime_foundation_matches(void)
{
    struct virtual_memory_mapping ignored;

    if (!permanent_arena_metadata_matches() ||
        active_layout.io_apic_count > ACPI_MAX_IO_APICS ||
        active_layout.local_apic_virtual_address !=
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE ||
        active_heap_mapped_pages > VM_HEAP_PAGE_COUNT ||
        active_task_stack_mapped_pages > VM_TASK_STACK_PAGE_COUNT ||
        (cpu_read_cr3() & VM_ENTRY_ADDRESS_MASK) !=
            active_layout.root_table_address ||
        (cpu_read_cr0() & VM_CR0_WRITE_PROTECT) == 0U ||
        (cpu_read_msr(VM_EFER_MSR) & VM_EFER_NXE) == 0U ||
        cpu_read_msr(VM_PAT_MSR) != VM_PAT_ARCHITECTURAL_DEFAULT ||
        !heap_window_layout_is_valid() ||
        !task_stack_window_layout_is_valid() ||
        !task_stack_window_matches(&permanent_arena) ||
        query_arena(
            &permanent_arena,
            VIRTUAL_MEMORY_HEAP_GUARD_BELOW,
            &ignored
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(
            &permanent_arena,
            VIRTUAL_MEMORY_HEAP_GUARD_ABOVE,
            &ignored
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        !mapping_matches(
            &permanent_arena,
            active_layout.local_apic_virtual_address,
            active_local_apic_physical,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        )) {
        return false;
    }

    for (size_t index = 0U; index < active_layout.io_apic_count; ++index) {
        if (active_layout.io_apic_virtual_addresses[index] !=
                VIRTUAL_MEMORY_DEVICE_WINDOW_BASE +
                    (index + 1U) * ZENITH_PAGE_SIZE ||
            !mapping_matches(
                &permanent_arena,
                active_layout.io_apic_virtual_addresses[index],
                active_io_apic_physical[index],
                VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
            )) {
            return false;
        }
    }

    return true;
}

static void layout_reset(struct virtual_memory_layout *layout)
{
    bytes_zero(layout, sizeof(*layout));
}

static void layout_copy(
    struct virtual_memory_layout *destination,
    const struct virtual_memory_layout *source
)
{
    destination->root_table_address = source->root_table_address;
    destination->local_apic_virtual_address =
        source->local_apic_virtual_address;
    destination->io_apic_count = source->io_apic_count;
    destination->table_page_count = source->table_page_count;

    for (size_t index = 0U; index < ACPI_MAX_IO_APICS; ++index) {
        destination->io_apic_virtual_addresses[index] =
            source->io_apic_virtual_addresses[index];
    }
}

static void active_state_reset(void)
{
    bytes_zero(&active_layout, sizeof(active_layout));
    bytes_zero(active_io_apic_physical, sizeof(active_io_apic_physical));
    active_local_apic_physical = 0U;
    active_heap_mapped_pages = 0U;
    active_task_stack_mapped_pages = 0U;
    test_heap_map_successes_remaining = 0U;
    test_heap_map_failure_enabled = false;
    test_heap_map_uncertain_failure_enabled = false;
    test_task_stack_map_successes_remaining = 0U;
    test_task_stack_map_failure_enabled = false;
    test_task_stack_map_uncertain_failure_enabled = false;
    virtual_memory_initialized = false;
}

static bool builder_self_test(void)
{
    struct vm_table_arena arena;
    struct virtual_memory_mapping mapping;
    enum virtual_memory_status status;
    uint64_t *leaf_entry;
    const uint64_t maximum_physical_mask = VM_ENTRY_ADDRESS_MASK;

    if (arena_initialize(NULL, test_table_pages, 1U,
            maximum_physical_mask) != VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT ||
        arena_initialize(&arena, NULL, 1U, maximum_physical_mask) !=
            VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT ||
        arena_initialize(&arena, test_table_pages, 0U,
            maximum_physical_mask) != VIRTUAL_MEMORY_STATUS_TABLE_LIMIT) {
        return false;
    }

    if (arena_initialize(
            &arena,
            test_table_pages,
            VM_TEST_TABLE_PAGE_CAPACITY,
            maximum_physical_mask
        ) != VIRTUAL_MEMORY_STATUS_OK) {
        return false;
    }

    if (map_page(&arena, UINT64_C(0x4000), UINT64_C(0x8000),
            VIRTUAL_MEMORY_EXECUTABLE) != VIRTUAL_MEMORY_STATUS_OK ||
        query_arena(&arena, UINT64_C(0x4123), &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != UINT64_C(0x8123) ||
        mapping.permissions != VIRTUAL_MEMORY_EXECUTABLE ||
        map_page(
            &arena,
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE,
            UINT64_C(0xFEE00000),
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        !mapping_matches(
            &arena,
            VIRTUAL_MEMORY_DEVICE_WINDOW_BASE,
            UINT64_C(0xFEE00000),
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        )) {
        return false;
    }

    if (map_page(&arena, UINT64_C(0x4000), UINT64_C(0x9000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT ||
        map_page(&arena, UINT64_C(0x4001), UINT64_C(0x9000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT ||
        map_page(&arena, UINT64_C(0x0000800000000000), UINT64_C(0x9000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS ||
        map_page(&arena, UINT64_C(0x9000), UINT64_C(0x0010000000000000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE ||
        map_page(&arena, UINT64_C(0x9000), UINT64_C(0xA000),
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_EXECUTABLE) !=
            VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS ||
        map_page(&arena, UINT64_C(0x9000), UINT64_C(0xA000),
            VIRTUAL_MEMORY_DEVICE) !=
            VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS ||
        query_arena(&arena, UINT64_C(0xA000), &mapping) !=
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        return false;
    }

    status = map_range(
        &arena,
        UINT64_C(0xFFFFFFFFFFFFF000),
        UINT64_C(0x1000),
        ZENITH_PAGE_SIZE * 2U,
        VIRTUAL_MEMORY_READ_ONLY
    );

    if (status != VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW ||
        map_range(&arena, UINT64_C(0x10000), UINT64_C(0x10000), 0U,
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT ||
        map_range(
            &arena,
            UINT64_C(0x10000),
            UINT64_C(0x10000),
            (VM_MAX_RANGE_PAGES + 1U) * ZENITH_PAGE_SIZE,
            VIRTUAL_MEMORY_READ_ONLY
        ) != VIRTUAL_MEMORY_STATUS_RANGE_TOO_LARGE) {
        return false;
    }

    if (arena_initialize(&arena, test_table_pages, 1U,
            maximum_physical_mask) != VIRTUAL_MEMORY_STATUS_OK ||
        map_page(&arena, UINT64_C(0x1000), UINT64_C(0x1000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_TABLE_LIMIT) {
        return false;
    }

    if (arena_initialize(
            &arena,
            permanent_table_pages,
            10U,
            maximum_physical_mask
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        prepare_heap_window(&arena) != VIRTUAL_MEMORY_STATUS_TABLE_LIMIT ||
        arena_initialize(
            &arena,
            permanent_table_pages,
            11U,
            maximum_physical_mask
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        prepare_heap_window(&arena) != VIRTUAL_MEMORY_STATUS_OK ||
        arena.used != 11U ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_HEAP_GUARD_BELOW,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_HEAP_GUARD_ABOVE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        map_page(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE,
            UINT64_C(0x200000),
            VIRTUAL_MEMORY_WRITABLE
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        map_page(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE,
            UINT64_C(0x201000),
            VIRTUAL_MEMORY_WRITABLE
        ) != VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT ||
        !mapping_matches(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE,
            UINT64_C(0x200000),
            VIRTUAL_MEMORY_WRITABLE
        )) {
        return false;
    }

    if (locate_leaf_entry(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE + ZENITH_PAGE_SIZE,
            &leaf_entry
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        *leaf_entry != 0U) {
        return false;
    }

    *leaf_entry = VM_ENTRY_WRITABLE;

    if (map_page(
            &arena,
            VIRTUAL_MEMORY_HEAP_BASE + ZENITH_PAGE_SIZE,
            UINT64_C(0x201000),
            VIRTUAL_MEMORY_WRITABLE
        ) != VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE) {
        return false;
    }

    *leaf_entry = 0U;

    if (arena_initialize(&arena, test_table_pages,
            VM_TEST_TABLE_PAGE_CAPACITY, maximum_physical_mask) !=
            VIRTUAL_MEMORY_STATUS_OK) {
        return false;
    }

    arena.pages[0][0] = VM_ENTRY_PRESENT | VM_ENTRY_WRITABLE | VM_ENTRY_HUGE;

    if (map_page(&arena, UINT64_C(0x1000), UINT64_C(0x1000),
            VIRTUAL_MEMORY_READ_ONLY) !=
            VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT ||
        arena_initialize(&arena, test_table_pages, 3U,
            maximum_physical_mask) != VIRTUAL_MEMORY_STATUS_OK ||
        prepare_task_stack_window(&arena) !=
            VIRTUAL_MEMORY_STATUS_TABLE_LIMIT ||
        arena_initialize(&arena, test_table_pages, 4U,
            maximum_physical_mask) != VIRTUAL_MEMORY_STATUS_OK ||
        prepare_task_stack_window(&arena) != VIRTUAL_MEMORY_STATUS_OK ||
        arena.used != 4U ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_TASK_STACK_BASE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_TASK_STACK_BASE + ZENITH_PAGE_SIZE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
        query_arena(
            &arena,
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END - ZENITH_PAGE_SIZE,
            &mapping
        ) != VIRTUAL_MEMORY_STATUS_NOT_MAPPED) {
        return false;
    }

    return true;
}

static bool device_policy_self_test(void)
{
    struct vm_table_arena arena;
    const uint64_t maximum_physical_mask = VM_ENTRY_ADDRESS_MASK;

    bytes_zero(&test_device_topology, sizeof(test_device_topology));
    layout_reset(&test_device_layout);
    test_device_topology.local_apic_address = UINT64_C(0xFEE00000);
    test_device_topology.io_apic_count = 1U;
    test_device_topology.io_apics[0].physical_address =
        UINT32_C(0xFEC00000);

    if (arena_initialize(
            &arena,
            test_table_pages,
            VM_TEST_TABLE_PAGE_CAPACITY,
            maximum_physical_mask
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        map_devices(
            &arena,
            &test_device_topology,
            &test_device_layout
        ) != VIRTUAL_MEMORY_STATUS_OK ||
        !mapping_matches(
            &arena,
            test_device_layout.local_apic_virtual_address,
            UINT64_C(0xFEE00000),
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        ) ||
        !mapping_matches(
            &arena,
            test_device_layout.io_apic_virtual_addresses[0],
            UINT64_C(0xFEC00000),
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        )) {
        return false;
    }

    bytes_zero(&test_device_topology, sizeof(test_device_topology));
    layout_reset(&test_device_layout);

    if (arena_initialize(&arena, test_table_pages,
            VM_TEST_TABLE_PAGE_CAPACITY, maximum_physical_mask) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        map_devices(&arena, &test_device_topology, &test_device_layout) !=
            VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS) {
        return false;
    }

    test_device_topology.local_apic_address = UINT64_C(0xFEE00001);

    if (map_devices(&arena, &test_device_topology, &test_device_layout) !=
            VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS) {
        return false;
    }

    test_device_topology.local_apic_address =
        UINT64_C(0x0010000000000000);

    if (map_devices(&arena, &test_device_topology, &test_device_layout) !=
            VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS) {
        return false;
    }

    test_device_topology.local_apic_address = UINT64_C(0xFEE00000);
    test_device_topology.io_apic_count = ACPI_MAX_IO_APICS + 1U;

    if (map_devices(&arena, &test_device_topology, &test_device_layout) !=
            VIRTUAL_MEMORY_STATUS_TOO_MANY_DEVICES) {
        return false;
    }

    test_device_topology.io_apic_count = 1U;
    test_device_topology.io_apics[0].physical_address =
        UINT32_C(0xFEC00001);
    return map_devices(&arena, &test_device_topology, &test_device_layout) ==
        VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS;
}

bool virtual_memory_self_test(void)
{
    return !virtual_memory_initialized && builder_self_test() &&
        device_policy_self_test();
}

enum virtual_memory_status virtual_memory_initialize(
    const struct acpi_topology *topology,
    struct virtual_memory_layout *layout
)
{
    struct virtual_memory_layout candidate;
    uint64_t physical_mask;
    uint64_t previous_pat;
    uintptr_t previous_cr3;
    bool pat_changed;
    enum virtual_memory_status status;

    if (topology == NULL || layout == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    layout_reset(layout);

    if (virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_ALREADY_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED;
    }

    if (!kernel_layout_is_valid() ||
        (cpu_read_cr0() & VM_CR0_WRITE_PROTECT) == 0U ||
        (cpu_read_msr(VM_EFER_MSR) & VM_EFER_NXE) == 0U) {
        return VIRTUAL_MEMORY_STATUS_BAD_KERNEL_LAYOUT;
    }

    status = physical_mask_detect(&physical_mask);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    status = arena_initialize(
        &permanent_arena,
        permanent_table_pages,
        VM_TABLE_PAGE_CAPACITY,
        physical_mask
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    layout_reset(&candidate);
    active_heap_mapped_pages = 0U;
    active_task_stack_mapped_pages = 0U;
    test_heap_map_successes_remaining = 0U;
    test_heap_map_failure_enabled = false;
    test_heap_map_uncertain_failure_enabled = false;
    test_task_stack_map_successes_remaining = 0U;
    test_task_stack_map_failure_enabled = false;
    test_task_stack_map_uncertain_failure_enabled = false;
    candidate.root_table_address = permanent_arena.root_address;
    status = map_kernel();

    if (status == VIRTUAL_MEMORY_STATUS_OK) {
        status = map_page(
            &permanent_arena,
            VM_VGA_ADDRESS,
            VM_VGA_ADDRESS,
            VIRTUAL_MEMORY_WRITABLE | VIRTUAL_MEMORY_DEVICE
        );
    }

    if (status == VIRTUAL_MEMORY_STATUS_OK) {
        status = map_devices(&permanent_arena, topology, &candidate);
    }

    if (status == VIRTUAL_MEMORY_STATUS_OK) {
        status = prepare_heap_window(&permanent_arena);
    }

    if (status == VIRTUAL_MEMORY_STATUS_OK) {
        status = prepare_task_stack_window(&permanent_arena);
    }

    candidate.table_page_count = permanent_arena.used;

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if (!layout_matches_topology(&permanent_arena, topology, &candidate)) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    status = pat_configure(&previous_pat, &pat_changed);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    previous_cr3 = cpu_read_cr3();
    cpu_write_cr3((uintptr_t)candidate.root_table_address);
    layout_copy(&active_layout, &candidate);
    active_local_apic_physical = topology->local_apic_address;

    for (size_t index = 0U; index < topology->io_apic_count; ++index) {
        active_io_apic_physical[index] =
            topology->io_apics[index].physical_address;
    }

    virtual_memory_initialized = true;

    if (virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK) {
        active_state_reset();
        cpu_write_cr3(previous_cr3);

        if (pat_changed) {
            pat_write_serialized(previous_pat);
        }

        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    layout_copy(layout, &candidate);
    return VIRTUAL_MEMORY_STATUS_OK;
}

enum virtual_memory_status virtual_memory_validate(void)
{
    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if ((cpu_read_cr3() & VM_ENTRY_ADDRESS_MASK) !=
            active_layout.root_table_address ||
        (cpu_read_cr0() & VM_CR0_WRITE_PROTECT) == 0U ||
        (cpu_read_msr(VM_EFER_MSR) & VM_EFER_NXE) == 0U ||
        cpu_read_msr(VM_PAT_MSR) != VM_PAT_ARCHITECTURAL_DEFAULT) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (!runtime_foundation_matches() || !active_layout_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

enum virtual_memory_status virtual_memory_query(
    uint64_t virtual_address,
    struct virtual_memory_mapping *mapping
)
{
    if (mapping == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (!permanent_arena_metadata_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    return query_arena(&permanent_arena, virtual_address, mapping);
}

enum virtual_memory_status virtual_memory_task_stack_bounds(
    size_t slot_index,
    uint64_t *lower_guard,
    uint64_t *payload_start,
    uint64_t *payload_end,
    uint64_t *upper_guard
)
{
    uint64_t slot_offset;

    if (lower_guard == NULL || payload_start == NULL ||
        payload_end == NULL || upper_guard == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    *lower_guard = 0U;
    *payload_start = 0U;
    *payload_end = 0U;
    *upper_guard = 0U;

    if (slot_index >= VIRTUAL_MEMORY_TASK_STACK_LIMIT) {
        return VIRTUAL_MEMORY_STATUS_BAD_STACK_SLOT;
    }

    slot_offset = (uint64_t)slot_index *
        VIRTUAL_MEMORY_TASK_STACK_SLOT_SIZE;

    if (slot_offset > UINT64_MAX - VIRTUAL_MEMORY_TASK_STACK_BASE) {
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    *lower_guard = VIRTUAL_MEMORY_TASK_STACK_BASE + slot_offset;

    if (*lower_guard > UINT64_MAX - ZENITH_PAGE_SIZE) {
        *lower_guard = 0U;
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    *payload_start = *lower_guard + ZENITH_PAGE_SIZE;

    if (*payload_start > UINT64_MAX -
            VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES * ZENITH_PAGE_SIZE) {
        *lower_guard = 0U;
        *payload_start = 0U;
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    *payload_end = *payload_start +
        VIRTUAL_MEMORY_TASK_STACK_PAYLOAD_PAGES * ZENITH_PAGE_SIZE;
    *upper_guard = *payload_end;

    if (*upper_guard > UINT64_MAX - ZENITH_PAGE_SIZE) {
        *lower_guard = 0U;
        *payload_start = 0U;
        *payload_end = 0U;
        *upper_guard = 0U;
        return VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    if (!address_is_page_aligned(*lower_guard) ||
        !address_is_page_aligned(*payload_start) ||
        !address_is_page_aligned(*payload_end) ||
        !address_is_page_aligned(*upper_guard) ||
        !address_is_canonical(*lower_guard) ||
        !address_is_canonical(*upper_guard + ZENITH_PAGE_SIZE - 1U) ||
        *lower_guard < VIRTUAL_MEMORY_TASK_STACK_BASE ||
        *upper_guard + ZENITH_PAGE_SIZE >
            VIRTUAL_MEMORY_TASK_STACK_WINDOW_END) {
        *lower_guard = 0U;
        *payload_start = 0U;
        *payload_end = 0U;
        *upper_guard = 0U;
        return VIRTUAL_MEMORY_STATUS_OUTSIDE_TASK_STACK_WINDOW;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

enum virtual_memory_status virtual_memory_query_task_stack_page(
    size_t slot_index,
    size_t page_index,
    struct virtual_memory_mapping *mapping
)
{
    uint64_t virtual_address;
    enum virtual_memory_status status;

    if (mapping == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    status = task_stack_page_address(slot_index, page_index, &virtual_address);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    return virtual_memory_query(virtual_address, mapping);
}

enum virtual_memory_status virtual_memory_map_task_stack_page(
    size_t slot_index,
    size_t page_index,
    uintptr_t physical_address
)
{
    struct virtual_memory_mapping mapping;
    uint64_t virtual_address;
    uint64_t *leaf_entry;
    uint64_t entry;
    enum virtual_memory_status status;

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED;
    }

    status = task_stack_page_address(slot_index, page_index, &virtual_address);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if (!address_is_page_aligned((uint64_t)physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (!address_fits_mask(
            (uint64_t)physical_address,
            permanent_arena.physical_mask
        )) {
        return VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE;
    }

    if (!runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    status = locate_leaf_entry(
        &permanent_arena,
        virtual_address,
        &leaf_entry
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if ((*leaf_entry & VM_ENTRY_PRESENT) != 0U) {
        return VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT;
    }

    if (*leaf_entry != 0U) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (test_task_stack_map_failure_enabled &&
        test_task_stack_map_successes_remaining == 0U) {
        test_task_stack_map_failure_enabled = false;
        return VIRTUAL_MEMORY_STATUS_TEST_FAILURE;
    }

    entry = (uint64_t)physical_address | VM_ENTRY_PRESENT |
        VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE;
    *leaf_entry = entry;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    ++active_task_stack_mapped_pages;

    if (test_task_stack_map_uncertain_failure_enabled) {
        test_task_stack_map_uncertain_failure_enabled = false;
        return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
    }

    if (query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        !runtime_foundation_matches()) {
        *leaf_entry = 0U;
        __asm__ volatile ("" : : : "memory");
        cpu_invalidate_page((uintptr_t)virtual_address);
        --active_task_stack_mapped_pages;

        if (query_arena(&permanent_arena, virtual_address, &mapping) !=
                VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
            !runtime_foundation_matches()) {
            return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
        }

        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (test_task_stack_map_failure_enabled) {
        --test_task_stack_map_successes_remaining;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

enum virtual_memory_status virtual_memory_unmap_task_stack_page(
    size_t slot_index,
    size_t page_index,
    uintptr_t expected_physical_address
)
{
    struct virtual_memory_mapping mapping;
    uint64_t virtual_address;
    uint64_t *leaf_entry;
    uint64_t saved_entry;
    enum virtual_memory_status status;

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED;
    }

    status = task_stack_page_address(slot_index, page_index, &virtual_address);

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if (!address_is_page_aligned((uint64_t)expected_physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (!address_fits_mask(
            (uint64_t)expected_physical_address,
            permanent_arena.physical_mask
        )) {
        return VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE;
    }

    if (!runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    status = locate_leaf_entry(
        &permanent_arena,
        virtual_address,
        &leaf_entry
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    saved_entry = *leaf_entry;

    if ((saved_entry & VM_ENTRY_PRESENT) == 0U) {
        return saved_entry == 0U
            ? VIRTUAL_MEMORY_STATUS_NOT_MAPPED
            : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (active_task_stack_mapped_pages == 0U) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if ((saved_entry & VM_ENTRY_ADDRESS_MASK) !=
            (uint64_t)expected_physical_address ||
        query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)expected_physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE) {
        return VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH;
    }

    *leaf_entry = 0U;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    --active_task_stack_mapped_pages;

    if (query_arena(&permanent_arena, virtual_address, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED &&
        runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_OK;
    }

    *leaf_entry = saved_entry;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    ++active_task_stack_mapped_pages;

    if (query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)expected_physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        !runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
    }

    return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
}

enum virtual_memory_status virtual_memory_map_heap_page(
    uint64_t virtual_address,
    uintptr_t physical_address
)
{
    struct virtual_memory_mapping mapping;
    uint64_t *leaf_entry;
    uint64_t entry;
    enum virtual_memory_status status;

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED;
    }

    if (!address_is_page_aligned(virtual_address) ||
        !address_is_page_aligned((uint64_t)physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (!address_is_canonical(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    if (!address_is_in_heap_window(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_OUTSIDE_HEAP_WINDOW;
    }

    if (!address_fits_mask(
            (uint64_t)physical_address,
            permanent_arena.physical_mask
        )) {
        return VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE;
    }

    if (!runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    status = locate_leaf_entry(
        &permanent_arena,
        virtual_address,
        &leaf_entry
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    if ((*leaf_entry & VM_ENTRY_PRESENT) != 0U) {
        return VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT;
    }

    if (*leaf_entry != 0U) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (test_heap_map_failure_enabled &&
        test_heap_map_successes_remaining == 0U) {
        test_heap_map_failure_enabled = false;
        return VIRTUAL_MEMORY_STATUS_TEST_FAILURE;
    }

    entry = (uint64_t)physical_address | VM_ENTRY_PRESENT |
        VM_ENTRY_WRITABLE | VM_ENTRY_NO_EXECUTE;
    *leaf_entry = entry;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    ++active_heap_mapped_pages;

    if (test_heap_map_uncertain_failure_enabled) {
        test_heap_map_uncertain_failure_enabled = false;
        return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
    }

    if (query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        !runtime_foundation_matches()) {
        *leaf_entry = 0U;
        __asm__ volatile ("" : : : "memory");
        cpu_invalidate_page((uintptr_t)virtual_address);
        --active_heap_mapped_pages;

        if (query_arena(&permanent_arena, virtual_address, &mapping) !=
                VIRTUAL_MEMORY_STATUS_NOT_MAPPED ||
            !runtime_foundation_matches()) {
            return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
        }

        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (test_heap_map_failure_enabled) {
        --test_heap_map_successes_remaining;
    }

    return VIRTUAL_MEMORY_STATUS_OK;
}

enum virtual_memory_status virtual_memory_unmap_heap_page(
    uint64_t virtual_address,
    uintptr_t expected_physical_address
)
{
    struct virtual_memory_mapping mapping;
    uint64_t *leaf_entry;
    uint64_t saved_entry;
    enum virtual_memory_status status;

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED;
    }

    if (!address_is_page_aligned(virtual_address) ||
        !address_is_page_aligned((uint64_t)expected_physical_address)) {
        return VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT;
    }

    if (!address_is_canonical(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS;
    }

    if (!address_is_in_heap_window(virtual_address)) {
        return VIRTUAL_MEMORY_STATUS_OUTSIDE_HEAP_WINDOW;
    }

    if (!address_fits_mask(
            (uint64_t)expected_physical_address,
            permanent_arena.physical_mask
        )) {
        return VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE;
    }

    if (!runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    status = locate_leaf_entry(
        &permanent_arena,
        virtual_address,
        &leaf_entry
    );

    if (status != VIRTUAL_MEMORY_STATUS_OK) {
        return status;
    }

    saved_entry = *leaf_entry;

    if ((saved_entry & VM_ENTRY_PRESENT) == 0U) {
        return saved_entry == 0U
            ? VIRTUAL_MEMORY_STATUS_NOT_MAPPED
            : VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if (active_heap_mapped_pages == 0U) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    if ((saved_entry & VM_ENTRY_ADDRESS_MASK) !=
            (uint64_t)expected_physical_address ||
        query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)expected_physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE) {
        return VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH;
    }

    *leaf_entry = 0U;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    --active_heap_mapped_pages;

    if (query_arena(&permanent_arena, virtual_address, &mapping) ==
            VIRTUAL_MEMORY_STATUS_NOT_MAPPED &&
        runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_OK;
    }

    *leaf_entry = saved_entry;
    __asm__ volatile ("" : : : "memory");
    cpu_invalidate_page((uintptr_t)virtual_address);
    ++active_heap_mapped_pages;

    if (query_arena(&permanent_arena, virtual_address, &mapping) !=
            VIRTUAL_MEMORY_STATUS_OK ||
        mapping.physical_address != (uint64_t)expected_physical_address ||
        mapping.permissions != VIRTUAL_MEMORY_WRITABLE ||
        !runtime_foundation_matches()) {
        return VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE;
    }

    return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
}

enum virtual_memory_status virtual_memory_runtime_get_stats(
    struct virtual_memory_runtime_stats *stats
)
{
    if (stats == NULL) {
        return VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT;
    }

    if (!virtual_memory_initialized) {
        return VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED;
    }

    if (virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK) {
        return VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE;
    }

    stats->heap_window_pages = VM_HEAP_PAGE_COUNT;
    stats->heap_mapped_pages = active_heap_mapped_pages;
    stats->task_stack_payload_pages = VM_TASK_STACK_PAGE_COUNT;
    stats->task_stack_mapped_pages = active_task_stack_mapped_pages;
    stats->table_pages_used = permanent_arena.used;
    stats->table_pages_capacity = permanent_arena.capacity;
    return VIRTUAL_MEMORY_STATUS_OK;
}

bool virtual_memory_test_fail_heap_map_after(size_t successful_maps)
{
    if (!virtual_memory_initialized || cpu_interrupts_enabled() ||
        test_heap_map_failure_enabled ||
        test_heap_map_uncertain_failure_enabled ||
        test_task_stack_map_failure_enabled ||
        test_task_stack_map_uncertain_failure_enabled) {
        return false;
    }

    test_heap_map_successes_remaining = successful_maps;
    test_heap_map_failure_enabled = true;
    return true;
}

bool virtual_memory_test_report_uncertain_heap_map_once(void)
{
    if (!virtual_memory_initialized || cpu_interrupts_enabled() ||
        test_heap_map_failure_enabled ||
        test_heap_map_uncertain_failure_enabled ||
        test_task_stack_map_failure_enabled ||
        test_task_stack_map_uncertain_failure_enabled) {
        return false;
    }

    test_heap_map_uncertain_failure_enabled = true;
    return true;
}

bool virtual_memory_test_fail_task_stack_map_after(size_t successful_maps)
{
    if (!virtual_memory_initialized || cpu_interrupts_enabled() ||
        test_task_stack_map_failure_enabled ||
        test_task_stack_map_uncertain_failure_enabled ||
        test_heap_map_failure_enabled ||
        test_heap_map_uncertain_failure_enabled) {
        return false;
    }

    test_task_stack_map_successes_remaining = successful_maps;
    test_task_stack_map_failure_enabled = true;
    return true;
}

bool virtual_memory_test_report_uncertain_task_stack_map_once(void)
{
    if (!virtual_memory_initialized || cpu_interrupts_enabled() ||
        test_task_stack_map_failure_enabled ||
        test_task_stack_map_uncertain_failure_enabled ||
        test_heap_map_failure_enabled ||
        test_heap_map_uncertain_failure_enabled) {
        return false;
    }

    test_task_stack_map_uncertain_failure_enabled = true;
    return true;
}

bool virtual_memory_ready(void)
{
    return virtual_memory_initialized;
}

const char *virtual_memory_status_string(enum virtual_memory_status status)
{
    switch (status) {
    case VIRTUAL_MEMORY_STATUS_OK:
        return "ok";
    case VIRTUAL_MEMORY_STATUS_NULL_ARGUMENT:
        return "null virtual-memory argument";
    case VIRTUAL_MEMORY_STATUS_ALREADY_INITIALIZED:
        return "virtual memory was initialized twice";
    case VIRTUAL_MEMORY_STATUS_NOT_INITIALIZED:
        return "virtual memory is not initialized";
    case VIRTUAL_MEMORY_STATUS_INTERRUPTS_ENABLED:
        return "virtual-memory mutation requires IF cleared";
    case VIRTUAL_MEMORY_STATUS_UNSUPPORTED_CPU:
        return "CPU lacks required PAT, NX, or physical-address support";
    case VIRTUAL_MEMORY_STATUS_BAD_KERNEL_LAYOUT:
        return "linked kernel layout cannot be mapped safely";
    case VIRTUAL_MEMORY_STATUS_BAD_ALIGNMENT:
        return "virtual-memory range is not page aligned";
    case VIRTUAL_MEMORY_STATUS_NONCANONICAL_ADDRESS:
        return "virtual address is not canonical in four-level paging";
    case VIRTUAL_MEMORY_STATUS_PHYSICAL_ADDRESS_TOO_WIDE:
        return "physical address exceeds the CPU or page-table width";
    case VIRTUAL_MEMORY_STATUS_RANGE_OVERFLOW:
        return "virtual-memory range arithmetic overflowed";
    case VIRTUAL_MEMORY_STATUS_RANGE_TOO_LARGE:
        return "virtual-memory range exceeds the mapping policy";
    case VIRTUAL_MEMORY_STATUS_BAD_PERMISSIONS:
        return "virtual-memory permissions violate W^X or device policy";
    case VIRTUAL_MEMORY_STATUS_TABLE_LIMIT:
        return "permanent page-table arena is exhausted";
    case VIRTUAL_MEMORY_STATUS_MAPPING_CONFLICT:
        return "virtual page is already mapped or uses a huge-page parent";
    case VIRTUAL_MEMORY_STATUS_NOT_MAPPED:
        return "virtual address is not mapped";
    case VIRTUAL_MEMORY_STATUS_OUTSIDE_HEAP_WINDOW:
        return "runtime mapping is outside the bounded heap window";
    case VIRTUAL_MEMORY_STATUS_OUTSIDE_TASK_STACK_WINDOW:
        return "runtime mapping is outside the bounded task-stack window";
    case VIRTUAL_MEMORY_STATUS_BAD_STACK_SLOT:
        return "task-stack slot index is outside the fixed limit";
    case VIRTUAL_MEMORY_STATUS_BAD_STACK_PAGE:
        return "task-stack page index is outside the payload";
    case VIRTUAL_MEMORY_STATUS_MAPPING_MISMATCH:
        return "runtime mapping does not match its expected frame";
    case VIRTUAL_MEMORY_STATUS_ROLLBACK_FAILURE:
        return "runtime mapping rollback could not restore ownership";
    case VIRTUAL_MEMORY_STATUS_TEST_FAILURE:
        return "injected runtime mapping failure";
    case VIRTUAL_MEMORY_STATUS_BAD_DEVICE_ADDRESS:
        return "device MMIO address cannot be mapped safely";
    case VIRTUAL_MEMORY_STATUS_TOO_MANY_DEVICES:
        return "device topology exceeds the permanent mapping window";
    case VIRTUAL_MEMORY_STATUS_PAT_FAILURE:
        return "CPU PAT configuration did not persist";
    case VIRTUAL_MEMORY_STATUS_VALIDATION_FAILURE:
        return "active virtual-memory contract failed validation";
    default:
        return "unknown virtual-memory status";
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/acpi.h>
#include <zenith/boot.h>
#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/heap.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/pic.h>
#include <zenith/pit.h>
#include <zenith/self_test.h>
#include <zenith/test.h>
#include <zenith/time.h>
#include <zenith/virtual_memory.h>

#define MAX_REPORTED_BOOT_LOADER_NAME 64U
#define KERNEL_TIMER_FREQUENCY_HZ UINT32_C(1000)
#define KERNEL_TIMER_PROOF_TICKS UINT64_C(8)

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

static struct acpi_topology acpi_topology;
static struct apic_configuration apic_configuration;
static struct virtual_memory_layout virtual_memory_layout;

static void report_boot_context(const struct boot_context *context)
{
    console_write("Zenith OS: boot loader: ");

    if (context->boot_loader_name == NULL) {
        console_write("unnamed");
    } else {
        size_t reported_length = context->boot_loader_name_length;

        if (reported_length > MAX_REPORTED_BOOT_LOADER_NAME) {
            reported_length = MAX_REPORTED_BOOT_LOADER_NAME;
        }

        console_write_n(context->boot_loader_name, reported_length);

        if (reported_length != context->boot_loader_name_length) {
            console_write("...");
        }
    }

    console_putc('\n');

    console_write("Zenith OS: memory map entries: ");
    console_write_u64(context->memory_map_entry_count);
    console_putc('\n');

    console_write("Zenith OS: reported usable bytes: ");
    console_write_u64(context->reported_usable_bytes);
    console_putc('\n');

    console_write("Zenith OS: highest reported address: ");
    console_write_hex(context->highest_reported_address);
    console_putc('\n');
}

static void report_allocator(const struct frame_allocator_stats *stats)
{
    console_write("Zenith OS: allocatable frames: ");
    console_write_u64(stats->allocatable_frames);
    console_putc('\n');

    console_write("Zenith OS: free frames: ");
    console_write_u64(stats->free_frames);
    console_putc('\n');

    console_write("Zenith OS: reserved frames: ");
    console_write_u64(stats->reserved_frames);
    console_putc('\n');

    console_write("Zenith OS: highest allocatable address: ");
    console_write_hex(stats->highest_allocatable_address);
    console_putc('\n');
}

static void report_acpi_root(const struct acpi_root *root)
{
    console_write("Zenith OS: ACPI ");
    console_write(acpi_root_kind_string(root->kind));
    console_write(" at ");
    console_write_hex(root->physical_address);
    console_write(" OEM ");
    console_write_n(root->oem_id, 6U);
    console_putc('\n');
}

static void report_acpi_madt(const struct acpi_madt *madt)
{
    console_write("Zenith OS: ACPI MADT at ");
    console_write_hex(madt->physical_address);
    console_write(" local APIC ");
    console_write_hex(madt->local_apic_address);
    console_write(" flags ");
    console_write_hex(madt->flags);
    console_putc('\n');

    console_write("Zenith OS: ACPI root entries: ");
    console_write_u64(madt->root_entry_count);
    console_write(" MADT OEM ");
    console_write_n(madt->oem_id, 6U);
    console_putc(' ');
    console_write_n(madt->oem_table_id, 8U);
    console_putc('\n');
}

static void report_acpi_topology(const struct acpi_topology *topology)
{
    console_write("Zenith OS: ACPI usable processors: ");
    console_write_u64(topology->processor_count);
    console_write(" enabled ");
    console_write_u64(topology->enabled_processor_count);
    console_write(" online-capable ");
    console_write_u64(topology->online_capable_processor_count);
    console_putc('\n');

    console_write("Zenith OS: ACPI I/O APICs: ");
    console_write_u64(topology->io_apic_count);
    console_write(" interrupt overrides ");
    console_write_u64(topology->interrupt_override_count);
    console_putc('\n');

    console_write("Zenith OS: ACPI effective local APIC: ");
    console_write_hex(topology->local_apic_address);

    if (topology->local_apic_address_overridden) {
        console_write(" (overridden)");
    }

    console_putc('\n');
}

static void report_virtual_memory(
    const struct virtual_memory_layout *layout
)
{
    console_write("Zenith OS: permanent CR3: ");
    console_write_hex(layout->root_table_address);
    console_write(" table pages ");
    console_write_u64(layout->table_page_count);
    console_putc('\n');

    console_write("Zenith OS: local APIC device window: ");
    console_write_hex(layout->local_apic_virtual_address);
    console_write(" I/O APIC mappings ");
    console_write_u64(layout->io_apic_count);
    console_putc('\n');
}

static void report_apic(const struct apic_configuration *configuration)
{
    console_write("Zenith OS: Local APIC ID ");
    console_write_u64(configuration->local_apic_id);
    console_write(" version ");
    console_write_hex(configuration->local_apic_version);
    console_putc('\n');

    console_write("Zenith OS: I/O APIC controllers ");
    console_write_u64(configuration->io_apic_count);
    console_write(" timer GSI ");
    console_write_u64(configuration->timer_global_interrupt);
    console_write(" input ");
    console_write_u64(configuration->timer_input);
    console_write(configuration->timer_active_low ?
        " active-low" : " active-high");
    console_write(configuration->timer_level_triggered ?
        " level" : " edge");
    console_putc('\n');
}

static void report_monotonic_time(void)
{
    console_write("Zenith OS: Local APIC periodic frequency ");
    console_write_u64(kernel_time_frequency_hz());
    console_write(" Hz period ");
    console_write_u64(kernel_time_period_nanoseconds());
    console_write(" ns\n");
}

static void prove_frame_lifecycle(void)
{
    uintptr_t first_frame;
    uintptr_t second_frame;
    uintptr_t heap_frame;
    uintptr_t rejected_frame = UINTPTR_MAX;
    enum frame_owner owner;
    enum frame_status status;

    status = frame_allocate(&first_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    status = frame_allocate(&second_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    if (first_frame == second_frame ||
        (first_frame & (ZENITH_PAGE_SIZE - 1U)) != 0U ||
        (second_frame & (ZENITH_PAGE_SIZE - 1U)) != 0U ||
        frame_query_owner(first_frame, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_GENERIC ||
        frame_query_owner(second_frame, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_GENERIC) {
        console_panic("frame allocator returned an invalid address");
    }

    if (frame_allocate_owned(FRAME_OWNER_NONE, &rejected_frame) !=
            FRAME_STATUS_INVALID_OWNER ||
        rejected_frame != UINTPTR_MAX ||
        frame_query_owner(first_frame, NULL) != FRAME_STATUS_NULL_ARGUMENT ||
        frame_query_owner(1U, &owner) != FRAME_STATUS_UNALIGNED_ADDRESS ||
        frame_query_owner(0U, &owner) !=
            FRAME_STATUS_FRAME_NOT_ALLOCATABLE) {
        console_panic("frame allocator failed owner rejection checks");
    }

    if (frame_allocate_owned(
            FRAME_OWNER_KERNEL_HEAP,
            &heap_frame
        ) != FRAME_STATUS_OK ||
        heap_frame == first_frame || heap_frame == second_frame ||
        frame_query_owner(heap_frame, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_KERNEL_HEAP ||
        frame_release(heap_frame) != FRAME_STATUS_OWNER_MISMATCH ||
        frame_release_owned(
            heap_frame,
            FRAME_OWNER_KERNEL_HEAP
        ) != FRAME_STATUS_OK) {
        console_panic("frame allocator failed heap owner lifecycle");
    }

    if (frame_release_owned(
            second_frame,
            FRAME_OWNER_KERNEL_HEAP
        ) != FRAME_STATUS_OWNER_MISMATCH ||
        frame_release_owned(
            second_frame,
            FRAME_OWNER_NONE
        ) != FRAME_STATUS_INVALID_OWNER) {
        console_panic("frame allocator failed owner release rejection");
    }

    console_write("Zenith OS: frame probe: ");
    console_write_hex(first_frame);
    console_write(" and ");
    console_write_hex(second_frame);
    console_putc('\n');

    status = frame_release(second_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    status = frame_release(first_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    if (frame_release(first_frame) != FRAME_STATUS_DOUBLE_FREE) {
        console_panic("frame allocator failed to reject a double free");
    }

    if (frame_query_owner(first_frame, &owner) != FRAME_STATUS_OK ||
        owner != FRAME_OWNER_NONE) {
        console_panic("released frame retained an owner");
    }

    if (frame_release(1U) != FRAME_STATUS_UNALIGNED_ADDRESS) {
        console_panic("frame allocator failed to reject an unaligned release");
    }

    if (frame_release(0U) != FRAME_STATUS_FRAME_NOT_ALLOCATABLE) {
        console_panic("frame allocator released permanently reserved memory");
    }
}

static void prove_heap_lifecycle(void)
{
    volatile uint8_t *allocation;
    void *raw_allocation;
    struct heap_stats stats;
    enum heap_status status = heap_allocate(257U, 64U, &raw_allocation);

    if (status != HEAP_STATUS_OK || raw_allocation == NULL ||
        ((uintptr_t)raw_allocation & UINT64_C(63)) != 0U) {
        console_panic("normal heap allocation proof failed");
    }

    allocation = (volatile uint8_t *)raw_allocation;

    for (size_t index = 0U; index < 257U; ++index) {
        allocation[index] = (uint8_t)(UINT8_C(0xA7) ^ (uint8_t)index);
    }

    for (size_t index = 0U; index < 257U; ++index) {
        if (allocation[index] !=
            (uint8_t)(UINT8_C(0xA7) ^ (uint8_t)index)) {
            console_panic("normal heap payload proof failed");
        }
    }

    status = heap_deallocate(raw_allocation);

    if (status != HEAP_STATUS_OK || heap_validate() != HEAP_STATUS_OK ||
        heap_get_stats(&stats) != HEAP_STATUS_OK ||
        stats.live_allocations != 0U || stats.reusable_bytes == 0U ||
        stats.mapped_pages != 1U) {
        console_panic("normal heap reuse proof failed");
    }
}

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information)
{
    struct acpi_madt acpi_madt;
    struct acpi_root acpi_root;
    struct boot_context context;
    struct frame_allocator_stats stats;
    enum boot_status boot_status;
    enum acpi_status acpi_status;
    enum apic_status apic_status;
    enum frame_status frame_status;
    enum interrupt_status interrupt_status;
    enum kernel_test_scenario test_scenario;
    enum pit_status pit_status;
    enum kernel_time_status time_status;
    uint64_t monotonic_before;
    uint64_t monotonic_after;
    uint64_t nanoseconds_before;
    uint64_t nanoseconds_after;
    uint64_t eoi_before;
    enum virtual_memory_status virtual_memory_status;

    console_initialize();
    interrupt_status = interrupts_initialize();

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        if (interrupt_status == INTERRUPT_STATUS_CPU_TABLE_FAILURE) {
            console_write("Zenith OS: CPU table detail: ");
            console_write(cpu_status_string(cpu_tables_validate()));
            console_putc('\n');
        }

        console_panic(interrupt_status_string(interrupt_status));
    }

    console_write("Zenith OS: kernel online\n");
    console_write("Zenith OS: descriptor tables verified\n");
    console_write("Zenith OS: interrupt foundation online\n");

    if (!boot_parser_self_test()) {
        console_panic("Multiboot2 parser self-test failed");
    }

    if (!acpi_self_test()) {
        console_panic("ACPI RSDP rejection self-test failed");
    }

    if (!acpi_tables_self_test()) {
        console_panic("ACPI table rejection self-test failed");
    }

    if (!acpi_topology_self_test()) {
        console_panic("ACPI MADT topology rejection self-test failed");
    }

    if (!virtual_memory_self_test()) {
        console_panic("virtual-memory rejection self-test failed");
    }

    if (!heap_self_test()) {
        console_panic("bounded-heap rejection self-test failed");
    }

    if (!apic_self_test()) {
        console_panic("APIC topology rejection self-test failed");
    }

    if (!apic_local_timer_self_test()) {
        console_panic("Local APIC timer arithmetic self-test failed");
    }

    if (!kernel_time_self_test()) {
        console_panic("monotonic-time arithmetic self-test failed");
    }

    console_write("Zenith OS: parser rejection tests passed\n");

    boot_status = boot_context_parse(magic, boot_information, &context);

    if (boot_status != BOOT_STATUS_OK) {
        console_panic(boot_status_string(boot_status));
    }

    acpi_status = acpi_root_discover(&context, &acpi_root);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    acpi_status = acpi_madt_discover(&acpi_root, &acpi_madt);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    acpi_status = acpi_topology_discover(&acpi_madt, &acpi_topology);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    report_boot_context(&context);
    report_acpi_root(&acpi_root);
    report_acpi_madt(&acpi_madt);
    report_acpi_topology(&acpi_topology);
    console_write("Zenith OS: ACPI root verified\n");
    console_write("Zenith OS: ACPI MADT verified\n");
    console_write("Zenith OS: ACPI MADT topology verified\n");
    frame_status = frame_allocator_initialize(&context);

    if (frame_status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(frame_status));
    }

    stats = frame_allocator_get_stats();
    report_allocator(&stats);
    prove_frame_lifecycle();

    stats = frame_allocator_get_stats();

    if (stats.allocated_frames != 0U ||
        stats.generic_allocated_frames != 0U ||
        stats.heap_allocated_frames != 0U ||
        frame_allocator_validate() != FRAME_STATUS_OK) {
        console_panic("frame lifecycle leaked a physical frame");
    }

    test_scenario = kernel_test_select(&context);
    virtual_memory_status = virtual_memory_initialize(
        &acpi_topology,
        &virtual_memory_layout
    );

    if (virtual_memory_status != VIRTUAL_MEMORY_STATUS_OK) {
        console_panic(virtual_memory_status_string(virtual_memory_status));
    }

    if (virtual_memory_validate() != VIRTUAL_MEMORY_STATUS_OK) {
        console_panic("permanent virtual-memory validation failed");
    }

    report_virtual_memory(&virtual_memory_layout);
    console_write("Zenith OS: virtual memory foundation passed\n");

    if (!interrupt_pic_spurious_self_test()) {
        console_panic("PIC spurious interrupt self-test failed");
    }

    apic_status = apic_initialize(
        &acpi_topology,
        &virtual_memory_layout,
        &apic_configuration
    );

    if (apic_status != APIC_STATUS_OK) {
        console_panic(apic_status_string(apic_status));
    }

    if (apic_validate() != APIC_STATUS_OK ||
        !apic_spurious_self_test()) {
        console_panic("active APIC validation failed");
    }

    report_apic(&apic_configuration);
    console_write("Zenith OS: legacy PIC permanently masked\n");
    console_write("Zenith OS: APIC interrupt routing verified\n");

    console_write("Zenith OS: day one passed\n");
    console_write("Zenith OS: memory foundation passed\n");
    kernel_test_run(test_scenario);

    if (!interrupt_breakpoint_self_test()) {
        console_panic("breakpoint register self-test failed");
    }

    if (!interrupt_ist_self_test()) {
        console_panic("IST routing self-test failed");
    }

    eoi_before = apic_eoi_count();
    pit_status = pit_start(UINT32_C(100));

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(UINT64_C(8));

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    if (pit_ticks() < UINT64_C(8) ||
        apic_eoi_count() < eoi_before + UINT64_C(8) ||
        !apic_timer_is_masked() || !pic_all_masked() ||
        apic_validate() != APIC_STATUS_OK) {
        console_panic("APIC-routed timer validation failed");
    }

    console_write("Zenith OS: exception probes passed\n");
    console_write("Zenith OS: legacy PIC spurious paths passed\n");
    console_write("Zenith OS: APIC-routed PIT delivered eight interrupts\n");
    console_write("Zenith OS: never triple fault milestone passed\n");

    if (test_scenario == KERNEL_TEST_HEAP &&
        (apic_test_discard_calibration_attempts(0U) ||
         apic_test_discard_calibration_attempts(3U) ||
         !apic_test_discard_calibration_attempts(1U))) {
        console_panic("bounded APIC calibration retry contract failed");
    }

    time_status = kernel_time_initialize(KERNEL_TIMER_FREQUENCY_HZ);

    if (time_status != KERNEL_TIME_STATUS_OK) {
        console_panic(kernel_time_status_string(time_status));
    }

    if (test_scenario == KERNEL_TEST_HEAP) {
        console_write("Zenith OS: APIC calibration retry verified\n");
    }

    monotonic_before = kernel_time_monotonic_ticks();
    time_status = kernel_time_monotonic_nanoseconds(&nanoseconds_before);

    if (time_status != KERNEL_TIME_STATUS_OK) {
        console_panic(kernel_time_status_string(time_status));
    }

    eoi_before = apic_eoi_count();
    time_status = kernel_time_wait_for_ticks(KERNEL_TIMER_PROOF_TICKS);

    if (time_status != KERNEL_TIME_STATUS_OK) {
        console_panic(kernel_time_status_string(time_status));
    }

    monotonic_after = kernel_time_monotonic_ticks();
    time_status = kernel_time_monotonic_nanoseconds(&nanoseconds_after);

    if (time_status != KERNEL_TIME_STATUS_OK) {
        console_panic(kernel_time_status_string(time_status));
    }

    if (monotonic_after < monotonic_before + KERNEL_TIMER_PROOF_TICKS ||
        nanoseconds_after <= nanoseconds_before ||
        nanoseconds_after - nanoseconds_before <
            KERNEL_TIMER_PROOF_TICKS *
                kernel_time_period_nanoseconds() ||
        apic_eoi_count() < eoi_before + KERNEL_TIMER_PROOF_TICKS ||
        pit_is_running() || !apic_timer_is_masked() ||
        !apic_local_timer_active() ||
        kernel_time_validate() != KERNEL_TIME_STATUS_OK) {
        console_panic("Local APIC monotonic timer validation failed");
    }

    report_monotonic_time();
    console_write("Zenith OS: Local APIC timer and monotonic clock verified\n");

    if (test_scenario == KERNEL_TEST_NONE ||
        test_scenario == KERNEL_TEST_NORMAL ||
        test_scenario == KERNEL_TEST_HEAP) {
        enum heap_status heap_status = heap_initialize();

        if (heap_status != HEAP_STATUS_OK) {
            console_panic(heap_status_string(heap_status));
        }

        if (test_scenario == KERNEL_TEST_HEAP) {
            kernel_test_complete_heap();
        }

        prove_heap_lifecycle();
        console_write("Zenith OS: bounded kernel heap verified\n");
    }

    if (test_scenario == KERNEL_TEST_LAPIC_TIMER) {
        kernel_test_complete_lapic_timer();
    }

    if (test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    console_halt();
}

/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <zenith/acpi.h>
#include <zenith/boot.h>
#include <zenith/console.h>
#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/memory.h>
#include <zenith/pit.h>
#include <zenith/self_test.h>
#include <zenith/test.h>

#define MAX_REPORTED_BOOT_LOADER_NAME 64U

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

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

static void prove_frame_lifecycle(void)
{
    uintptr_t first_frame;
    uintptr_t second_frame;
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
        (second_frame & (ZENITH_PAGE_SIZE - 1U)) != 0U) {
        console_panic("frame allocator returned an invalid address");
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

    if (frame_release(1U) != FRAME_STATUS_UNALIGNED_ADDRESS) {
        console_panic("frame allocator failed to reject an unaligned release");
    }

    if (frame_release(0U) != FRAME_STATUS_FRAME_NOT_ALLOCATABLE) {
        console_panic("frame allocator released permanently reserved memory");
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
    enum frame_status frame_status;
    enum interrupt_status interrupt_status;
    enum kernel_test_scenario test_scenario;
    enum pit_status pit_status;

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

    report_boot_context(&context);
    report_acpi_root(&acpi_root);
    report_acpi_madt(&acpi_madt);
    console_write("Zenith OS: ACPI root verified\n");
    console_write("Zenith OS: ACPI MADT verified\n");
    frame_status = frame_allocator_initialize(&context);

    if (frame_status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(frame_status));
    }

    stats = frame_allocator_get_stats();
    report_allocator(&stats);
    prove_frame_lifecycle();

    stats = frame_allocator_get_stats();

    if (stats.allocated_frames != 0U) {
        console_panic("frame lifecycle leaked a physical frame");
    }

    console_write("Zenith OS: day one passed\n");
    console_write("Zenith OS: memory foundation passed\n");
    test_scenario = kernel_test_select(&context);
    kernel_test_run(test_scenario);

    if (!interrupt_breakpoint_self_test()) {
        console_panic("breakpoint register self-test failed");
    }

    if (!interrupt_ist_self_test()) {
        console_panic("IST routing self-test failed");
    }

    if (!interrupt_pic_spurious_self_test()) {
        console_panic("PIC spurious interrupt self-test failed");
    }

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

    console_write("Zenith OS: exception probes passed\n");
    console_write("Zenith OS: PIC spurious paths passed\n");
    console_write("Zenith OS: PIT delivered eight interrupts\n");
    console_write("Zenith OS: never triple fault milestone passed\n");

    if (test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    console_halt();
}

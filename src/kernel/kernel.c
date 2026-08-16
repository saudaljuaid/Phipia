/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/boot.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/self_test.h>
#include <seneri/test.h>

#define MAX_REPORTED_BOOT_LOADER_NAME 64U

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

/*
 * The discovered interrupt topology outlives kernel_main's frame and is larger
 * than early boot should place on the 16 KiB kernel stack.
 */
static struct acpi_topology boot_topology;

static void report_boot_context(const struct boot_context *context)
{
    console_write("Seneri OS: boot loader: ");

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

    console_write("Seneri OS: memory map entries: ");
    console_write_u64(context->memory_map_entry_count);
    console_putc('\n');

    console_write("Seneri OS: reported usable bytes: ");
    console_write_u64(context->reported_usable_bytes);
    console_putc('\n');

    console_write("Seneri OS: highest reported address: ");
    console_write_hex(context->highest_reported_address);
    console_putc('\n');
}

static void report_allocator(const struct frame_allocator_stats *stats)
{
    console_write("Seneri OS: allocatable frames: ");
    console_write_u64(stats->allocatable_frames);
    console_putc('\n');

    console_write("Seneri OS: free frames: ");
    console_write_u64(stats->free_frames);
    console_putc('\n');

    console_write("Seneri OS: reserved frames: ");
    console_write_u64(stats->reserved_frames);
    console_putc('\n');

    console_write("Seneri OS: highest allocatable address: ");
    console_write_hex(stats->highest_allocatable_address);
    console_putc('\n');
}

static void report_acpi_root(const struct acpi_root *root)
{
    console_write("Seneri OS: ACPI ");
    console_write(acpi_root_kind_string(root->kind));
    console_write(" at ");
    console_write_hex(root->physical_address);
    console_write(" OEM ");
    console_write_n(root->oem_id, 6U);
    console_putc('\n');
}

static void report_acpi_madt(const struct acpi_madt *madt)
{
    console_write("Seneri OS: ACPI MADT at ");
    console_write_hex(madt->physical_address);
    console_write(" local APIC ");
    console_write_hex(madt->local_apic_address);
    console_write(" flags ");
    console_write_hex(madt->flags);
    console_putc('\n');

    console_write("Seneri OS: ACPI root entries: ");
    console_write_u64(madt->root_entry_count);
    console_write(" MADT OEM ");
    console_write_n(madt->oem_id, 6U);
    console_putc(' ');
    console_write_n(madt->oem_table_id, 8U);
    console_putc('\n');
}

static void report_acpi_topology(const struct acpi_topology *topology)
{
    console_write("Seneri OS: ACPI local APIC base ");
    console_write_hex(topology->local_apic_address);

    if (topology->local_apic_address_overridden) {
        console_write(" overridden");
    }

    console_putc('\n');

    console_write("Seneri OS: ACPI processors: ");
    console_write_u64(topology->local_apic_count);
    console_write(" enabled ");
    console_write_u64(topology->enabled_processor_count);
    console_write(" NMI entries ");
    console_write_u64(topology->nmi_entry_count);
    console_write(" unmodelled ");
    console_write_u64(topology->ignored_entry_count);
    console_putc('\n');

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *io_apic = &topology->io_apics[index];

        console_write("Seneri OS: ACPI I/O APIC id ");
        console_write_u64(io_apic->identifier);
        console_write(" at ");
        console_write_hex(io_apic->address);
        console_write(" base GSI ");
        console_write_u64(io_apic->interrupt_base);
        console_putc('\n');
    }

    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];

        console_write("Seneri OS: ACPI override ISA IRQ ");
        console_write_u64(override->source);
        console_write(" to GSI ");
        console_write_u64(override->global_system_interrupt);
        console_write(" flags ");
        console_write_hex(override->flags);
        console_putc('\n');
    }
}

static void report_apic(const struct apic_state *apic)
{
    console_write("Seneri OS: local APIC id ");
    console_write_u64(apic->id);
    console_write(" version ");
    console_write_hex(apic->version);
    console_write(" LVT entries ");
    console_write_u64((uint64_t)apic->max_lvt_entry + 1U);
    console_write(" at ");
    console_write_hex(apic->base_address);
    console_putc('\n');

    console_write("Seneri OS: local APIC legacy routing ");
    console_write(apic->legacy_interrupts_routed ? "LINT0 ExtINT" : "masked");
    console_putc('\n');
}

static void report_ioapic(const struct ioapic_state *ioapic)
{
    for (size_t index = 0; index < ioapic->count; ++index) {
        const struct ioapic_unit *unit = &ioapic->units[index];

        console_write("Seneri OS: I/O APIC id ");
        console_write_u64(unit->identifier);
        console_write(" version ");
        console_write_hex(unit->version);
        console_write(" entries ");
        console_write_u64(unit->entry_count);
        console_write(" base GSI ");
        console_write_u64(unit->interrupt_base);
        console_putc('\n');
    }
}

/*
 * The same timer, counted over both delivery paths. Proving the legacy path
 * still works after the I/O APIC path is programmed is what keeps this
 * increment reversible.
 */
static void prove_timer_route(enum pit_route route)
{
    enum pit_status pit_status = pit_start(UINT32_C(100), route);

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

    if (pit_ticks() < UINT64_C(8)) {
        console_panic("timer route delivered too few interrupts");
    }
}

/*
 * Retire the inherited interrupt path once the discovered one has been proved.
 * The 8259 pair is masked and latched shut, and the local APIC stops carrying
 * its output, so nothing can reach the processor except through the I/O APIC.
 */
static void retire_legacy_interrupt_path(void)
{
    const enum pic_status pic_status = pic_retire();
    enum apic_status apic_status;

    if (pic_status != PIC_STATUS_OK) {
        console_panic(pic_status_string(pic_status));
    }

    apic_status = apic_retire_legacy_routing();

    if (apic_status != APIC_STATUS_OK) {
        console_panic(apic_status_string(apic_status));
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF) ||
        pic_set_mask(0U, false) != PIC_STATUS_RETIRED) {
        console_panic("legacy PIC did not stay retired");
    }
}

/*
 * Calibrate the local APIC timer against the PIT and run it. The PIT stays for
 * exactly this reason: the APIC timer's input clock rate is not reported
 * anywhere, so it can only become a clock by being counted against one.
 */
static void prove_apic_timer(void)
{
    enum apic_timer_status status = apic_timer_calibrate();

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    console_write("Seneri OS: local APIC timer calibrated at ");
    console_write_u64(apic_timer_counts_per_second());
    console_write(" counts per second\n");

    status = apic_timer_start(UINT32_C(100));

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    status = apic_timer_wait_for_ticks(UINT64_C(8));

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    status = apic_timer_stop();

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    if (apic_timer_ticks() < UINT64_C(8)) {
        console_panic("local APIC timer delivered too few interrupts");
    }
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
        (first_frame & (SENERI_PAGE_SIZE - 1U)) != 0U ||
        (second_frame & (SENERI_PAGE_SIZE - 1U)) != 0U) {
        console_panic("frame allocator returned an invalid address");
    }

    console_write("Seneri OS: frame probe: ");
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
    struct apic_state apic_state;
    struct boot_context context;
    struct frame_allocator_stats stats;
    struct ioapic_state ioapic_state;
    enum boot_status boot_status;
    enum acpi_status acpi_status;
    enum apic_status apic_status;
    enum ioapic_status ioapic_status;
    enum frame_status frame_status;
    enum interrupt_status interrupt_status;
    enum kernel_test_scenario test_scenario;

    console_initialize();
    interrupt_status = interrupts_initialize();

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        if (interrupt_status == INTERRUPT_STATUS_CPU_TABLE_FAILURE) {
            console_write("Seneri OS: CPU table detail: ");
            console_write(cpu_status_string(cpu_tables_validate()));
            console_putc('\n');
        }

        console_panic(interrupt_status_string(interrupt_status));
    }

    console_write("Seneri OS: kernel online\n");
    console_write("Seneri OS: descriptor tables verified\n");
    console_write("Seneri OS: interrupt foundation online\n");

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
        console_panic("ACPI topology rejection self-test failed");
    }

    if (!apic_self_test()) {
        console_panic("local APIC rejection self-test failed");
    }

    if (!ioapic_self_test()) {
        console_panic("I/O APIC routing self-test failed");
    }

    if (!apic_timer_self_test()) {
        console_panic("local APIC timer calibration self-test failed");
    }

    console_write("Seneri OS: parser rejection tests passed\n");

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

    acpi_status = acpi_topology_discover(&acpi_madt, &boot_topology);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    report_boot_context(&context);
    report_acpi_root(&acpi_root);
    report_acpi_madt(&acpi_madt);
    report_acpi_topology(&boot_topology);
    console_write("Seneri OS: ACPI root verified\n");
    console_write("Seneri OS: ACPI MADT verified\n");
    console_write("Seneri OS: ACPI topology verified\n");
    apic_status = apic_bring_online(&boot_topology);

    if (apic_status != APIC_STATUS_OK) {
        console_panic(apic_status_string(apic_status));
    }

    apic_state = apic_get_state();
    report_apic(&apic_state);
    console_write("Seneri OS: local APIC online\n");
    ioapic_status = ioapic_initialize(&boot_topology);

    if (ioapic_status != IOAPIC_STATUS_OK) {
        console_panic(ioapic_status_string(ioapic_status));
    }

    ioapic_state = ioapic_get_state();
    report_ioapic(&ioapic_state);
    console_write("Seneri OS: I/O APIC online\n");
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

    console_write("Seneri OS: day one passed\n");
    console_write("Seneri OS: memory foundation passed\n");
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

    prove_timer_route(PIT_ROUTE_LEGACY_PIC);
    prove_timer_route(PIT_ROUTE_IO_APIC);
    retire_legacy_interrupt_path();
    prove_timer_route(PIT_ROUTE_IO_APIC);
    prove_apic_timer();

    console_write("Seneri OS: exception probes passed\n");
    console_write("Seneri OS: PIC spurious paths passed\n");
    console_write("Seneri OS: PIT delivered eight interrupts\n");
    console_write("Seneri OS: I/O APIC delivered eight interrupts\n");
    console_write("Seneri OS: legacy 8259 retired\n");
    console_write("Seneri OS: timer survives legacy retirement\n");
    console_write("Seneri OS: local APIC timer delivered eight interrupts\n");
    console_write("Seneri OS: never triple fault milestone passed\n");

    if (test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    console_halt();
}

/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The installed Pyrenis Boot Ledger plan.
 *
 * Every function that performs migrated boot work is private to this file and
 * can only be reached through a typed descriptor. kernel_main constructs,
 * validates and executes the ledger; it does not know a subsystem call order.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pyrenis/acpi.h>
#include <pyrenis/apic.h>
#include <pyrenis/apic_timer.h>
#include <pyrenis/boot.h>
#include <pyrenis/boot_ledger.h>
#include <pyrenis/boot_plan.h>
#include <pyrenis/boot_stages.h>
#include <pyrenis/clock.h>
#include <pyrenis/console.h>
#include <pyrenis/cpu.h>
#include <pyrenis/device_substrate.h>
#include <pyrenis/dma.h>
#include <pyrenis/framebuffer.h>
#include <pyrenis/heap.h>
#include <pyrenis/interrupts.h>
#include <pyrenis/interrupt_vector.h>
#include <pyrenis/font.h>
#include <pyrenis/logo.h>
#include <pyrenis/ioapic.h>
#include <pyrenis/keyboard.h>
#include <pyrenis/memory.h>
#include <pyrenis/msix.h>
#include <pyrenis/paging.h>
#include <pyrenis/pci.h>
#include <pyrenis/pci_resource.h>
#include <pyrenis/pointer.h>
#include <pyrenis/pm_timer.h>
#include <pyrenis/screen.h>
#include <pyrenis/self_test.h>
#include <pyrenis/shell.h>
#include <pyrenis/surface.h>
#include <pyrenis/test.h>
#include <pyrenis/thread.h>
#include <pyrenis/timer.h>
#include <pyrenis/tsc.h>
#include <pyrenis/ui.h>
#include <pyrenis/ui_font.h>

static void stage_failed(
    struct boot_context *context,
    struct boot_stage_result *result,
    const char *detail
)
{
    context->stage_failure_detail = detail;
    boot_stage_result_fail(result);
}

static enum paging_status add_boot_window(
    struct paging_device_windows *windows,
    enum paging_device_window_kind kind,
    uint32_t instance,
    uint64_t base,
    uint64_t length,
    enum paging_memory_type memory_type
)
{
    return paging_device_windows_add(windows, kind, instance, base, length,
        memory_type, PAGING_DEVICE_WINDOW_WRITE);
}

static enum paging_status add_optional_boot_window(
    struct paging_device_windows *windows,
    enum paging_device_window_kind kind,
    uint64_t base,
    uint64_t length,
    enum paging_memory_type memory_type
)
{
    enum paging_status status = add_boot_window(windows, kind, 0U, base,
        length, memory_type);

    if (status == PAGING_STATUS_OK) {
        status = paging_device_windows_validate(windows, windows);

        if (status != PAGING_STATUS_OK) {
            windows->count -= 1U;
        }
    }

    return status;
}

static void report_optional_window_refusal(
    enum paging_device_window_kind kind,
    enum paging_status status
)
{
    console_write("Pyrenis: ");
    console_write(paging_device_window_kind_string(kind));
    console_write(" unavailable: ");
    console_write(paging_status_string(status));
    console_putc('\n');
}

static enum paging_status construct_device_windows(
    struct boot_context *context
)
{
    struct boot_framebuffer *framebuffer = &context->information.framebuffer;
    struct paging_device_windows *windows = &context->device_windows;
    const struct acpi_mcfg *mcfg = context->mcfg_present ?
        &context->acpi_mcfg : NULL;
    bool framebuffer_registered = false;
    enum paging_status status;

    paging_device_windows_reset(windows);
    status = add_boot_window(windows, PAGING_DEVICE_WINDOW_VGA_TEXT, 0U,
        PAGING_VGA_TEXT_BUFFER_BASE, PAGING_PAGE_SIZE,
        PAGING_MEMORY_UNCACHEABLE);

    if (status == PAGING_STATUS_OK) {
        status = add_boot_window(windows, PAGING_DEVICE_WINDOW_LOCAL_APIC, 0U,
            context->topology.local_apic_address, PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE);
    }

    for (size_t index = 0U;
         status == PAGING_STATUS_OK &&
            index < context->topology.io_apic_count;
         ++index) {
        status = add_boot_window(windows, PAGING_DEVICE_WINDOW_IO_APIC,
            (uint32_t)index, context->topology.io_apics[index].address,
            PAGING_PAGE_SIZE, PAGING_MEMORY_UNCACHEABLE);
    }

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    if (mcfg != NULL && mcfg->allocation_count != 0U) {
        const uint64_t base = mcfg->allocations[0].base_address;

        if (base == 0U ||
            base > PYRENIS_EARLY_PHYSICAL_LIMIT - PAGING_ECAM_WINDOW_SIZE) {
            report_optional_window_refusal(PAGING_DEVICE_WINDOW_PCI_ECAM,
                PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE);
        } else if ((base & (PAGING_HUGE_PAGE_SIZE - 1U)) != 0U) {
            report_optional_window_refusal(PAGING_DEVICE_WINDOW_PCI_ECAM,
                PAGING_STATUS_UNALIGNED_DEVICE_WINDOW);
        } else {
            const enum paging_status optional_status =
                add_optional_boot_window(windows,
                    PAGING_DEVICE_WINDOW_PCI_ECAM, base,
                    PAGING_ECAM_WINDOW_SIZE, PAGING_MEMORY_UNCACHEABLE);

            if (optional_status != PAGING_STATUS_OK) {
                report_optional_window_refusal(PAGING_DEVICE_WINDOW_PCI_ECAM,
                    optional_status);
                context->mcfg_present = false;
            }
        }
    }

    if (framebuffer->present) {
        uint64_t page_base = 0U;
        uint64_t page_end = 0U;
        uint64_t region_base = 0U;
        uint64_t region_end = 0U;

        if (framebuffer->size == 0U) {
            report_optional_window_refusal(PAGING_DEVICE_WINDOW_FRAMEBUFFER,
                PAGING_STATUS_ZERO_LENGTH_DEVICE_WINDOW);
        } else if (framebuffer->size > UINT64_MAX - framebuffer->address) {
            report_optional_window_refusal(PAGING_DEVICE_WINDOW_FRAMEBUFFER,
                PAGING_STATUS_DEVICE_WINDOW_RANGE_OVERFLOW);
        } else {
            const uint64_t framebuffer_end =
                framebuffer->address + framebuffer->size;

            if (framebuffer_end > PYRENIS_EARLY_PHYSICAL_LIMIT) {
                report_optional_window_refusal(
                    PAGING_DEVICE_WINDOW_FRAMEBUFFER,
                    PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE);
            } else {
                page_base = framebuffer->address &
                    ~(PAGING_PAGE_SIZE - 1U);
                page_end = (framebuffer_end + PAGING_PAGE_SIZE - 1U) &
                    ~(PAGING_PAGE_SIZE - 1U);
                region_base = framebuffer->address &
                    ~(PAGING_HUGE_PAGE_SIZE - 1U);
                region_end = (framebuffer_end + PAGING_HUGE_PAGE_SIZE - 1U) &
                    ~(PAGING_HUGE_PAGE_SIZE - 1U);

                if (region_end - region_base >
                    PAGING_DEVICE_WINDOW_MAX_LENGTH) {
                    report_optional_window_refusal(
                        PAGING_DEVICE_WINDOW_FRAMEBUFFER,
                        PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE);
                } else {
                    const enum paging_status optional_status =
                        add_optional_boot_window(windows,
                            PAGING_DEVICE_WINDOW_FRAMEBUFFER, page_base,
                            page_end - page_base,
                            PAGING_MEMORY_WRITE_COMBINING);

                    if (optional_status != PAGING_STATUS_OK) {
                        report_optional_window_refusal(
                            PAGING_DEVICE_WINDOW_FRAMEBUFFER,
                            optional_status);
                    } else {
                        framebuffer_registered = true;
                    }
                }
            }
        }
    }

    framebuffer->present = framebuffer_registered;
    return paging_device_windows_validate(windows, windows);
}

static void execute_early_serial(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_succeed(descriptor, result);
}

static void execute_interrupt_foundation(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum interrupt_status status = interrupts_initialize();

    if (status != INTERRUPT_STATUS_OK) {
        if (status == INTERRUPT_STATUS_CPU_TABLE_FAILURE) {
            console_write("Pyrenis: CPU table detail: ");
            console_write(cpu_status_string(cpu_tables_validate()));
            console_putc('\n');
        }

        stage_failed(context, result, interrupt_status_string(status));
        return;
    }

    console_write("Pyrenis: kernel online\n");
    console_write("Pyrenis: descriptor tables verified\n");
    console_write("Pyrenis: interrupt foundation online\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_pure_self_tests(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const char *failure = NULL;

    if (!boot_parser_self_test()) {
        failure = "Multiboot2 parser self-test failed";
    } else if (!acpi_self_test()) {
        failure = "ACPI RSDP rejection self-test failed";
    } else if (!acpi_tables_self_test()) {
        failure = "ACPI table rejection self-test failed";
    } else if (!acpi_topology_self_test()) {
        failure = "ACPI topology rejection self-test failed";
    } else if (!apic_self_test()) {
        failure = "local APIC rejection self-test failed";
    } else if (!ioapic_self_test()) {
        failure = "I/O APIC routing self-test failed";
    } else if (!apic_timer_self_test()) {
        failure = "local APIC timer calibration self-test failed";
    } else if (!tsc_self_test()) {
        failure = "TSC conversion self-test failed";
    } else if (!pm_timer_self_test()) {
        failure = "ACPI PM timer arithmetic self-test failed";
    } else if (!clock_self_test()) {
        failure = "monotonic clock self-test failed";
    } else if (!timer_self_test()) {
        failure = "deadline timer table self-test failed";
    } else if (!paging_self_test()) {
        failure = "page table arithmetic self-test failed";
    } else if (!heap_self_test()) {
        failure = "kernel heap block table self-test failed";
    } else if (!pci_self_test()) {
        failure = "PCI configuration arithmetic self-test failed";
    } else if (!thread_self_test()) {
        failure = "thread table and stack layout self-test failed";
    } else if (!framebuffer_self_test()) {
        failure = "framebuffer geometry self-test failed";
    } else if (!surface_self_test()) {
        failure = "surface primitive self-test failed";
    } else if (!screen_self_test()) {
        failure = "screen console grid self-test failed";
    } else if (!keyboard_self_test()) {
        failure = "keyboard translation self-test failed";
    } else if (!shell_self_test()) {
        failure = "shell line and dispatch self-test failed";
    } else if (!pointer_self_test()) {
        failure = pointer_self_test_failure();
    } else if (!ui_font_self_test()) {
        failure = ui_font_self_test_failure();
    } else if (!ui_self_test()) {
        failure = ui_self_test_failure();
    } else if (pyrenis_logo_self_test() != 1) {
        failure = "logo decoder self-test failed";
    } else if (pyrenis_font_self_test() != 1) {
        failure = "font reader self-test failed";
    }

    if (failure != NULL) {
        stage_failed(context, result, failure);
        return;
    }

    console_write("Pyrenis: parser rejection tests passed\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_boot_information(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum boot_status status = boot_information_parse(
        context->multiboot_magic, context->multiboot_information_address,
        &context->information);

    if (status != BOOT_STATUS_OK) {
        stage_failed(context, result, boot_status_string(status));
        return;
    }

    boot_stage_result_succeed(descriptor, result);
}

static void execute_firmware_discovery(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    enum acpi_status status;

    status = acpi_root_discover(&context->information, &context->acpi_root);
    if (status == ACPI_STATUS_OK) {
        status = acpi_madt_discover(&context->acpi_root,
            &context->acpi_madt);
    }
    if (status == ACPI_STATUS_OK) {
        status = acpi_topology_discover(&context->acpi_madt,
            &context->topology);
    }
    if (status == ACPI_STATUS_OK) {
        status = acpi_fadt_discover(&context->acpi_root,
            &context->acpi_fadt);
    }

    if (status != ACPI_STATUS_OK) {
        stage_failed(context, result, acpi_status_string(status));
        return;
    }

    status = acpi_mcfg_discover(&context->acpi_root, &context->acpi_mcfg);

    if (status == ACPI_STATUS_MISSING_MCFG) {
        context->mcfg_present = false;
    } else if (status != ACPI_STATUS_OK) {
        stage_failed(context, result, acpi_status_string(status));
        return;
    } else {
        context->mcfg_present = true;
    }

    boot_stage_result_succeed(descriptor, result);
}

static void execute_device_windows(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum paging_status status = construct_device_windows(context);

    if (status != PAGING_STATUS_OK) {
        stage_failed(context, result, paging_status_string(status));
        return;
    }

    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = context->device_windows.count;
    result->proof_counter_count = 1U;
}

static void execute_interrupt_controllers(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct pm_timer_state pm_timer_state;
    struct apic_state apic_state;
    struct ioapic_state ioapic_state;
    enum pm_timer_status pm_status;
    enum apic_status apic_status;
    enum ioapic_status ioapic_status;

    pm_status = pm_timer_initialize(&context->acpi_fadt);
    if (pm_status != PM_TIMER_STATUS_OK) {
        stage_failed(context, result, pm_timer_status_string(pm_status));
        return;
    }

    report_boot_information(&context->information);
    report_acpi_root(&context->acpi_root);
    report_acpi_madt(&context->acpi_madt);
    report_acpi_topology(&context->topology);
    pm_timer_state = pm_timer_get_state();
    report_acpi_fadt(&context->acpi_fadt);
    report_pm_timer(&pm_timer_state);
    report_acpi_mcfg(&context->acpi_mcfg, context->mcfg_present);
    console_write("Pyrenis: ACPI root verified\n");
    console_write("Pyrenis: ACPI MADT verified\n");
    console_write("Pyrenis: ACPI topology verified\n");
    console_write("Pyrenis: ACPI FADT verified\n");
    console_write("Pyrenis: ACPI configuration windows verified\n");

    apic_status = apic_bring_online(&context->topology);
    if (apic_status != APIC_STATUS_OK) {
        stage_failed(context, result, apic_status_string(apic_status));
        return;
    }

    apic_state = apic_get_state();
    report_apic(&apic_state);
    console_write("Pyrenis: local APIC online\n");
    ioapic_status = ioapic_initialize(&context->topology);

    if (ioapic_status != IOAPIC_STATUS_OK) {
        stage_failed(context, result, ioapic_status_string(ioapic_status));
        return;
    }

    ioapic_state = ioapic_get_state();
    report_ioapic(&ioapic_state);
    console_write("Pyrenis: I/O APIC online\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_frame_allocator(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct frame_allocator_stats stats;
    const enum frame_status status = frame_allocator_initialize(
        &context->information);

    if (status != FRAME_STATUS_OK) {
        stage_failed(context, result, frame_status_string(status));
        return;
    }

    stats = frame_allocator_get_stats();
    report_allocator(&stats);
    prove_frame_lifecycle();
    stats = frame_allocator_get_stats();

    if (stats.allocated_frames != 0U) {
        stage_failed(context, result,
            "frame lifecycle leaked a physical frame");
        return;
    }

    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = stats.allocatable_frames;
    result->proof_counter_count = 1U;
}

static void execute_paging_install(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct paging_state paging;

    install_page_tables(&context->device_windows);
    paging = paging_get_state();
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = paging.table_frames;
    result->proof_counters[1] = context->device_windows.count;
    result->proof_counter_count = 2U;
}

static void execute_paging_proofs(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct paging_audit audit;
    size_t failed_window = 0U;
    enum paging_status status = paging_verify();

    if (status == PAGING_STATUS_OK) {
        status = paging_audit_hierarchy(&audit);
    }

    if (status == PAGING_STATUS_OK && audit.write_execute_leaves != 0U) {
        stage_failed(context, result,
            "installed paging hierarchy contains a W+X leaf");
        return;
    }

    if (status == PAGING_STATUS_OK) {
        status = paging_verify_device_windows(&context->device_windows,
            &failed_window);
    }

    if (status != PAGING_STATUS_OK) {
        stage_failed(context, result, paging_status_string(status));
        return;
    }

    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = audit.leaf_count;
    result->proof_counters[1] = context->device_windows.count;
    result->proof_counter_count = 2U;
}

static void execute_framebuffer_wc(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    if (!context->information.framebuffer.present) {
        boot_stage_result_skip(descriptor, result);
        return;
    }

    prove_write_combining(&context->topology,
        context->mcfg_present ? &context->acpi_mcfg : NULL,
        &context->information.framebuffer);
    boot_stage_result_succeed(descriptor, result);
}

static void execute_memory_runtime(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_paging_lifecycle();
    bring_up_heap();
    prove_heap_lifecycle();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_framebuffer_output(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    prove_framebuffer(&context->information.framebuffer);

    if (!framebuffer_is_active()) {
        stage_failed(context, result,
            "framebuffer output did not establish a surface");
        return;
    }

    prove_surface();
    draw_logo();
    prove_screen_console();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_keyboard(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_keyboard();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_shell(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_shell();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_ui_font(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum ui_font_status status = ui_font_initialize();

    if (status != UI_FONT_STATUS_OK) {
        stage_failed(context, result, ui_font_status_string(status));
        return;
    }

    console_write("Pyrenis: First Light font verified\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = pyrenis_ui_font_size();
    result->proof_counters[1] = pyrenis_ui_font_fingerprint();
    result->proof_counter_count = 2U;
}

static void execute_pointer_decision(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum pointer_status status = pointer_initialize();

    (void)context;
    if (status == POINTER_STATUS_OK) {
        console_write("Pyrenis: PS/2 pointer available\n");
    } else {
        console_write("Pyrenis: PS/2 pointer unavailable: ");
        console_write(pointer_status_string(status));
        console_putc('\n');
    }

    boot_stage_result_succeed(descriptor, result);
}

static void execute_pointer_outcome(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;

    if (pointer_is_present()) {
        boot_stage_result_succeed(descriptor, result);
    } else {
        boot_stage_result_skip(descriptor, result);
    }
}

static void execute_ui_layout(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    struct ui_layout layout;
    enum ui_status status;

    status = ui_layout_build(framebuffer.width, framebuffer.height, &layout);
    if (status == UI_STATUS_OK) {
        status = ui_layout_validate(&layout);
    }

    if (status != UI_STATUS_OK) {
        stage_failed(context, result, ui_status_string(status));
        return;
    }

    console_write("Pyrenis: First Light layout validated\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = framebuffer.width;
    result->proof_counters[1] = framebuffer.height;
    result->proof_counter_count = 2U;
}

static void execute_early_scenario(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    console_write("Pyrenis: day one passed\n");
    console_write("Pyrenis: memory foundation passed\n");
    context->test_scenario = kernel_test_select(&context->information);
    context->test_context.mcfg = context->mcfg_present ?
        &context->acpi_mcfg : NULL;
    context->test_context.framebuffer = &context->information.framebuffer;
    context->test_context.device_windows = &context->device_windows;
    context->test_context.mcfg_present = context->mcfg_present;
    kernel_test_run(context->test_scenario, &context->test_context);
    boot_stage_result_succeed(descriptor, result);
}

static void execute_interrupt_proofs(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    if (!interrupt_breakpoint_self_test()) {
        stage_failed(context, result, "breakpoint register self-test failed");
        return;
    }

    if (!interrupt_ist_self_test()) {
        stage_failed(context, result, "IST routing self-test failed");
        return;
    }

    if (!interrupt_pic_spurious_self_test()) {
        stage_failed(context, result,
            "PIC spurious interrupt self-test failed");
        return;
    }

    boot_stage_result_succeed(descriptor, result);
}

static void execute_timer_routing(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_timer_route(PIT_ROUTE_LEGACY_PIC);
    prove_timer_route(PIT_ROUTE_IO_APIC);
    retire_legacy_interrupt_path();
    prove_timer_route(PIT_ROUTE_IO_APIC);
    prove_level_route();
    prove_pm_timer();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_timer_calibration(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_apic_timer();
    prove_tsc();
    retire_pit();
    prove_clocks_without_pit();
    prove_monotonic_time();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_pci(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    bring_up_pci(context->mcfg_present ? &context->acpi_mcfg : NULL,
        context->mcfg_present);
    boot_stage_result_succeed(descriptor, result);
}

static const struct pci_function *resource_probe_function(void)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL &&
            (function->header_type == PCI_HEADER_TYPE_ENDPOINT ||
                function->header_type == PCI_HEADER_TYPE_BRIDGE)) {
            return function;
        }
    }
    return NULL;
}

static void execute_pci_resource_foundation(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const struct pci_function *probe = resource_probe_function();
    const enum pci_resource_status status = pci_resource_initialize();

    if (status != PCI_RESOURCE_STATUS_OK) {
        stage_failed(context, result, pci_resource_status_string(status));
        return;
    }
    if (!pci_resource_self_test(probe)) {
        stage_failed(context, result,
            "PCI BAR transaction negative controls failed");
        return;
    }
    console_write("Pyrenis: PCI resource ownership negative controls 4/4 passed\n");
    console_write("Pyrenis: supervisor NX UC device-MMIO arena established\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] =
        pci_resource_get_state().arena_pages;
    result->proof_counter_count = 1U;
}

static void execute_dynamic_vector_foundation(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum interrupt_vector_status status = interrupt_vector_initialize();

    if (status != INTERRUPT_VECTOR_STATUS_OK) {
        stage_failed(context, result,
            interrupt_vector_status_string(status));
        return;
    }
    if (!interrupt_vector_self_test() || !msix_self_test()) {
        stage_failed(context, result,
            "dynamic vector or MSI-X negative controls failed");
        return;
    }
    console_write("Pyrenis: dynamic vector negative controls 4/4 passed\n");
    console_write("Pyrenis: dynamic interrupt vector foundation established\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = interrupt_vector_get_state().capacity;
    result->proof_counter_count = 1U;
}

static void execute_dma_foundation(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum dma_status status = dma_initialize();

    if (status != DMA_STATUS_OK) {
        stage_failed(context, result, dma_status_string(status));
        return;
    }
    if (!dma_self_test()) {
        stage_failed(context, result, "DMA ownership negative controls failed");
        return;
    }
    console_write("Pyrenis: bounded DMA negative controls 2/2 passed\n");
    console_write("Pyrenis: contiguous DMA ownership foundation established\n");
    boot_stage_result_succeed(descriptor, result);
}

static bool device_proof_dependencies_complete(
    const struct boot_stage_descriptor *descriptor
)
{
    static const enum boot_capability required[] = {
        BOOT_CAPABILITY_PAGE_TABLES_INSTALLED,
        BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE,
        BOOT_CAPABILITY_HEAP_AVAILABLE,
        BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE,
        BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED,
        BOOT_CAPABILITY_INTERRUPTS_ENABLED,
        BOOT_CAPABILITY_THREADING_AVAILABLE,
        BOOT_CAPABILITY_SCHEDULER_AVAILABLE,
        BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE,
        BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE
    };

    if (descriptor == NULL || descriptor->required_capability_count !=
            sizeof(required) / sizeof(required[0])) {
        return false;
    }
    for (size_t required_index = 0U;
         required_index < sizeof(required) / sizeof(required[0]);
         ++required_index) {
        bool found = false;
        for (size_t declared_index = 0U;
             declared_index < descriptor->required_capability_count;
             ++declared_index) {
            if (descriptor->required_capabilities[declared_index] ==
                    required[required_index]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static void execute_device_substrate_proof(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct boot_stage_descriptor missing_count;
    struct boot_stage_descriptor missing_member;
    struct device_substrate_proof proof;
    enum device_substrate_status status;

    if (!device_proof_dependencies_complete(descriptor)) {
        stage_failed(context, result,
            "device-substrate proof prerequisite set is incomplete");
        return;
    }
    missing_count = *descriptor;
    --missing_count.required_capability_count;
    missing_member = *descriptor;
    missing_member.required_capabilities[
        missing_member.required_capability_count - 1U] =
            missing_member.required_capabilities[0];
    if (device_proof_dependencies_complete(&missing_count) ||
        device_proof_dependencies_complete(&missing_member) ||
        !kernel_test_device_substrate_exit_self_test()) {
        stage_failed(context, result,
            "device-substrate contract negative controls failed");
        return;
    }

    status = device_substrate_prove(&proof);
    if (status == DEVICE_SUBSTRATE_STATUS_ABSENT) {
        console_write("Pyrenis: device-substrate fixture absent\n");
        boot_stage_result_skip(descriptor, result);
        return;
    }
    if (status != DEVICE_SUBSTRATE_STATUS_OK) {
        const struct pci_function *function = pci_find_device(
            UINT16_C(0x1AF4), UINT16_C(0x1044));

        console_write("Pyrenis: PCI ");
        if (function != NULL) {
            console_write_u64(function->address.segment);
            console_putc(':');
            console_write_u64(function->address.bus);
            console_putc(':');
            console_write_u64(function->address.device);
            console_putc('.');
            console_write_u64(function->address.function);
        } else {
            console_write("unknown");
        }
        console_write(" operation device-substrate proof violated invariant: ");
        console_write(device_substrate_status_string(status));
        console_putc('\n');
        stage_failed(context, result, device_substrate_status_string(status));
        return;
    }

    console_write("Pyrenis: VirtIO RNG device DMA wrote ");
    console_write_u64(proof.random_bytes);
    console_write(" bytes; nonzero ");
    console_write_u64(proof.nonzero_bytes);
    console_putc('\n');
    console_write("Pyrenis: MSI-X delivered ");
    console_write_u64(proof.interrupt_count);
    console_write(" interrupt; used ring ");
    console_write_u64(proof.used_before);
    console_write(" -> ");
    console_write_u64(proof.used_after);
    console_putc('\n');
    console_write("Pyrenis: device substrate teardown complete\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = proof.interrupt_count;
    result->proof_counters[1] = proof.random_bytes;
    result->proof_counter_count = 2U;
}

static void execute_threading(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_threads();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_scheduler(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    prove_preemption();
    boot_stage_result_succeed(descriptor, result);
}

static void execute_closing_proofs(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    enum paging_status paging_status = paging_verify();
    enum heap_status heap_status;
    enum pci_status pci_status;
    enum pci_resource_status pci_resource_status;
    enum dma_status dma_status;

    if (paging_status != PAGING_STATUS_OK) {
        stage_failed(context, result, paging_status_string(paging_status));
        return;
    }

    heap_status = heap_verify();
    if (heap_status != HEAP_STATUS_OK) {
        stage_failed(context, result, heap_status_string(heap_status));
        return;
    }

    pci_status = pci_verify();
    if (pci_status != PCI_STATUS_OK) {
        stage_failed(context, result, pci_status_string(pci_status));
        return;
    }

    pci_resource_status = pci_resource_verify();
    if (pci_resource_status != PCI_RESOURCE_STATUS_OK) {
        stage_failed(context, result,
            pci_resource_status_string(pci_resource_status));
        return;
    }

    dma_status = dma_verify();
    if (dma_status != DMA_STATUS_OK) {
        stage_failed(context, result, dma_status_string(dma_status));
        return;
    }

    const struct pci_resource_state resource_state =
        pci_resource_get_state();
    const struct dma_state installed_dma_state = dma_get_state();
    const struct interrupt_vector_state vector_state =
        interrupt_vector_get_state();
    const struct msix_state installed_msix_state = msix_get_state();
    if (resource_state.active_claims != 0U ||
        resource_state.active_mappings != 0U ||
        resource_state.mapped_pages != 0U ||
        resource_state.bus_masters != 0U ||
        installed_dma_state.active_allocations != 0U ||
        installed_dma_state.cpu_owned_allocations != 0U ||
        installed_dma_state.device_owned_allocations != 0U ||
        vector_state.allocated != 0U ||
        installed_msix_state.active_bindings != 0U ||
        installed_msix_state.failure_injection_armed) {
        stage_failed(context, result,
            "device foundation ownership leaked across teardown");
        return;
    }

    if (framebuffer_is_active()) {
        const enum framebuffer_status framebuffer_status =
            framebuffer_verify();

        if (framebuffer_status != FRAMEBUFFER_STATUS_OK) {
            stage_failed(context, result,
                framebuffer_status_string(framebuffer_status));
            return;
        }
    }

    console_write("Pyrenis: exception probes passed\n");
    console_write("Pyrenis: PIC spurious paths passed\n");
    console_write("Pyrenis: PIT delivered eight interrupts\n");
    console_write("Pyrenis: I/O APIC delivered eight interrupts\n");
    console_write("Pyrenis: legacy 8259 retired\n");
    console_write("Pyrenis: timer survives legacy retirement\n");
    console_write(
        "Pyrenis: I/O APIC delivered eight level-triggered interrupts\n"
    );
    console_write("Pyrenis: level-triggered routing established\n");
    console_write("Pyrenis: local APIC timer delivered eight interrupts\n");
    console_write("Pyrenis: TSC reference established\n");
    console_write("Pyrenis: PM timer independent reference established\n");
    console_write("Pyrenis: PIT retired\n");
    console_write("Pyrenis: clocks survive PIT retirement\n");
    console_write("Pyrenis: deadline timers online\n");
    console_write("Pyrenis: monotonic time established\n");
    console_write("Pyrenis: virtual memory established\n");
    console_write("Pyrenis: kernel heap established\n");
    console_write("Pyrenis: PCI enumeration established\n");
    console_write("Pyrenis: device foundations established\n");
    console_write("Pyrenis: kernel threads passed\n");
    console_write("Pyrenis: preemption passed\n");
    if (framebuffer_is_active()) {
        console_write("Pyrenis: framebuffer passed\n");
        console_write("Pyrenis: logo passed\n");
        console_write("Pyrenis: screen console passed\n");
        console_write("Pyrenis: shell passed\n");
    }
    console_write("Pyrenis: keyboard passed\n");
    console_write("Pyrenis: never triple fault milestone passed\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_desktop_construction(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum ui_status status = ui_construct(pointer_is_present());

    if (status != UI_STATUS_OK) {
        stage_failed(context, result, ui_status_string(status));
        return;
    }

    console_write("Pyrenis: First Light desktop constructed\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_desktop_activation(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    const enum ui_status status = ui_activate();

    if (status != UI_STATUS_OK) {
        stage_failed(context, result, ui_status_string(status));
        return;
    }

    console_write("Pyrenis: First Light desktop activated\n");
    boot_stage_result_succeed(descriptor, result);
}

static void execute_first_light_proof(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    struct ui_proof proof;
    const enum ui_status status = ui_verify_installed(&proof);

    if (status != UI_STATUS_OK) {
        stage_failed(context, result, ui_status_string(status));
        return;
    }

    console_write("Pyrenis: First Light installed proof passed\n");
    boot_stage_result_succeed(descriptor, result);
    result->proof_counters[0] = proof.render_hash;
    result->proof_counters[1] = proof.glyphs;
    result->proof_counter_count = 2U;
}

#define REQUIRED_STAGE(identifier, label, boot_phase, irreversible, function) \
    { \
        .id = identifier, \
        .name = label, \
        .required = true, \
        .phase = boot_phase, \
        .irreversible_class = irreversible, \
        .execute = function \
    }

#define OPTIONAL_STAGE(identifier, label, boot_phase, irreversible, function) \
    { \
        .id = identifier, \
        .name = label, \
        .required = false, \
        .phase = boot_phase, \
        .irreversible_class = irreversible, \
        .execute = function \
    }

#define OPTIONAL_NEUTRAL_STAGE(identifier, label, boot_phase, irreversible, \
    function) \
    { \
        .id = identifier, \
        .name = label, \
        .required = false, \
        .skip_preserves_health = true, \
        .phase = boot_phase, \
        .irreversible_class = irreversible, \
        .execute = function \
    }

static const struct boot_stage_descriptor installed_descriptors[] = {
    REQUIRED_STAGE(BOOT_STAGE_EARLY_SERIAL, "early serial",
        BOOT_PHASE_FOUNDATION, BOOT_IRREVERSIBLE_NONE, execute_early_serial),
    REQUIRED_STAGE(BOOT_STAGE_INTERRUPT_FOUNDATION, "interrupt foundation",
        BOOT_PHASE_FOUNDATION, BOOT_IRREVERSIBLE_NONE,
        execute_interrupt_foundation),
    REQUIRED_STAGE(BOOT_STAGE_PURE_SELF_TESTS, "pure boot self-tests",
        BOOT_PHASE_FOUNDATION, BOOT_IRREVERSIBLE_NONE,
        execute_pure_self_tests),
    REQUIRED_STAGE(BOOT_STAGE_BOOT_INFORMATION, "boot information",
        BOOT_PHASE_DISCOVERY, BOOT_IRREVERSIBLE_NONE,
        execute_boot_information),
    REQUIRED_STAGE(BOOT_STAGE_FIRMWARE_DISCOVERY, "firmware discovery",
        BOOT_PHASE_DISCOVERY, BOOT_IRREVERSIBLE_NONE,
        execute_firmware_discovery),
    REQUIRED_STAGE(BOOT_STAGE_DEVICE_WINDOWS, "device-window registry",
        BOOT_PHASE_DISCOVERY, BOOT_IRREVERSIBLE_NONE,
        execute_device_windows),
    REQUIRED_STAGE(BOOT_STAGE_INTERRUPT_CONTROLLERS,
        "interrupt controllers", BOOT_PHASE_CONTROLLERS,
        BOOT_IRREVERSIBLE_NONE, execute_interrupt_controllers),
    REQUIRED_STAGE(BOOT_STAGE_FRAME_ALLOCATOR, "physical frame allocator",
        BOOT_PHASE_CONTROLLERS, BOOT_IRREVERSIBLE_NONE,
        execute_frame_allocator),
    REQUIRED_STAGE(BOOT_STAGE_PAGING_INSTALL,
        "PAT and page-table installation", BOOT_PHASE_MEMORY_TRANSITION,
        BOOT_IRREVERSIBLE_PAT_CR3, execute_paging_install),
    REQUIRED_STAGE(BOOT_STAGE_PAGING_PROOFS, "installed paging proofs",
        BOOT_PHASE_MEMORY_TRANSITION, BOOT_IRREVERSIBLE_NONE,
        execute_paging_proofs),
    OPTIONAL_STAGE(BOOT_STAGE_FRAMEBUFFER_WC,
        "independent framebuffer WC proof", BOOT_PHASE_MEMORY_TRANSITION,
        BOOT_IRREVERSIBLE_NONE, execute_framebuffer_wc),
    REQUIRED_STAGE(BOOT_STAGE_MEMORY_RUNTIME, "heap and paging runtime",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_NONE, execute_memory_runtime),
    OPTIONAL_STAGE(BOOT_STAGE_FRAMEBUFFER_OUTPUT, "framebuffer output",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_FRAMEBUFFER_OUTPUT,
        execute_framebuffer_output),
    REQUIRED_STAGE(BOOT_STAGE_KEYBOARD, "keyboard interrupt path",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_INTERRUPT_ENABLE,
        execute_keyboard),
    OPTIONAL_STAGE(BOOT_STAGE_SHELL, "interactive shell",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_NONE, execute_shell),
    OPTIONAL_STAGE(BOOT_STAGE_UI_FONT, "First Light UI font",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_NONE, execute_ui_font),
    OPTIONAL_STAGE(BOOT_STAGE_POINTER_DECISION,
        "pointer availability decision", BOOT_PHASE_RUNTIME,
        BOOT_IRREVERSIBLE_NONE, execute_pointer_decision),
    OPTIONAL_NEUTRAL_STAGE(BOOT_STAGE_POINTER_OUTCOME,
        "pointer availability outcome", BOOT_PHASE_RUNTIME,
        BOOT_IRREVERSIBLE_NONE, execute_pointer_outcome),
    OPTIONAL_STAGE(BOOT_STAGE_UI_LAYOUT, "First Light layout",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_NONE, execute_ui_layout),
    REQUIRED_STAGE(BOOT_STAGE_EARLY_SCENARIO, "early scenario gate",
        BOOT_PHASE_RUNTIME, BOOT_IRREVERSIBLE_NONE, execute_early_scenario),
    REQUIRED_STAGE(BOOT_STAGE_INTERRUPT_PROOFS, "interrupt proofs",
        BOOT_PHASE_TIMERS, BOOT_IRREVERSIBLE_NONE, execute_interrupt_proofs),
    REQUIRED_STAGE(BOOT_STAGE_TIMER_ROUTING, "interrupt routing",
        BOOT_PHASE_TIMERS, BOOT_IRREVERSIBLE_NONE, execute_timer_routing),
    REQUIRED_STAGE(BOOT_STAGE_TIMER_CALIBRATION, "timer calibration",
        BOOT_PHASE_TIMERS, BOOT_IRREVERSIBLE_APIC_TIMER,
        execute_timer_calibration),
    REQUIRED_STAGE(BOOT_STAGE_PCI, "PCI access", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_NONE, execute_pci),
    REQUIRED_STAGE(BOOT_STAGE_PCI_RESOURCE_FOUNDATION,
        "PCI resource ownership", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_NONE, execute_pci_resource_foundation),
    REQUIRED_STAGE(BOOT_STAGE_DYNAMIC_VECTOR_FOUNDATION,
        "dynamic interrupt vectors", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_NONE, execute_dynamic_vector_foundation),
    REQUIRED_STAGE(BOOT_STAGE_DMA_FOUNDATION, "DMA foundation",
        BOOT_PHASE_SERVICES, BOOT_IRREVERSIBLE_NONE,
        execute_dma_foundation),
    REQUIRED_STAGE(BOOT_STAGE_THREADING, "threading", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_NONE, execute_threading),
    REQUIRED_STAGE(BOOT_STAGE_SCHEDULER, "scheduler", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_SCHEDULER, execute_scheduler),
    OPTIONAL_NEUTRAL_STAGE(BOOT_STAGE_DEVICE_SUBSTRATE_PROOF,
        "installed device-substrate proof", BOOT_PHASE_SERVICES,
        BOOT_IRREVERSIBLE_NONE, execute_device_substrate_proof),
    REQUIRED_STAGE(BOOT_STAGE_CLOSING_PROOFS, "closing boot proofs",
        BOOT_PHASE_PROOFS, BOOT_IRREVERSIBLE_NONE, execute_closing_proofs),
    OPTIONAL_STAGE(BOOT_STAGE_DESKTOP_CONSTRUCTION, "desktop construction",
        BOOT_PHASE_PROOFS, BOOT_IRREVERSIBLE_NONE,
        execute_desktop_construction),
    OPTIONAL_STAGE(BOOT_STAGE_DESKTOP_ACTIVATION, "desktop activation",
        BOOT_PHASE_PROOFS, BOOT_IRREVERSIBLE_NONE,
        execute_desktop_activation),
    OPTIONAL_STAGE(BOOT_STAGE_FIRST_LIGHT_PROOF,
        "First Light installed proof", BOOT_PHASE_PROOFS,
        BOOT_IRREVERSIBLE_NONE, execute_first_light_proof)
};

_Static_assert(sizeof(installed_descriptors) /
    sizeof(installed_descriptors[0]) <= BOOT_LEDGER_STAGE_CAPACITY,
    "installed boot plan exceeds the ledger capacity");

static bool declare_dependencies(
    struct boot_stage_descriptor *descriptor
)
{
    switch (descriptor->id) {
    case BOOT_STAGE_EARLY_SERIAL:
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_INTERRUPT_FOUNDATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_PURE_SELF_TESTS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_BOOT_SELF_TESTS_COMPLETE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_BOOT_INFORMATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_BOOT_SELF_TESTS_COMPLETE;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_FIRMWARE_DISCOVERY:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_ACPI_ROOT_VALIDATED;
        descriptor->provided_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_TOPOLOGY_DISCOVERED;
        descriptor->provided_capabilities[2] =
            BOOT_CAPABILITY_CLOCKS_DISCOVERED;
        descriptor->provided_capability_count = 3U;
        break;
    case BOOT_STAGE_DEVICE_WINDOWS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_TOPOLOGY_DISCOVERED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED;
        descriptor->provided_capabilities[1] =
            BOOT_CAPABILITY_FRAMEBUFFER_AVAILABILITY_DECIDED;
        descriptor->provided_capability_count = 2U;
        break;
    case BOOT_STAGE_INTERRUPT_CONTROLLERS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_ACPI_ROOT_VALIDATED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_TOPOLOGY_DISCOVERED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_CLOCKS_DISCOVERED;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED;
        descriptor->required_capability_count = 4U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_FRAME_ALLOCATOR:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_PAGING_INSTALL:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_PAGING_PROOFS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED;
        descriptor->provided_capabilities[1] =
            BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED;
        descriptor->provided_capability_count = 2U;
        break;
    case BOOT_STAGE_FRAMEBUFFER_WC:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_FRAMEBUFFER_AVAILABILITY_DECIDED;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
        descriptor->provided_capability_count = 1U;
        descriptor->skipped_capabilities[0] =
            BOOT_CAPABILITY_FRAMEBUFFER_SERIAL_FALLBACK;
        descriptor->skipped_capability_count = 1U;
        break;
    case BOOT_STAGE_MEMORY_RUNTIME:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_FRAMEBUFFER_OUTPUT:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->provided_capabilities[1] =
            BOOT_CAPABILITY_FRAMEBUFFER_OUTPUT_INSTALLED;
        descriptor->provided_capability_count = 2U;
        break;
    case BOOT_STAGE_KEYBOARD:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->provided_capabilities[1] =
            BOOT_CAPABILITY_KEYBOARD_AVAILABLE;
        descriptor->provided_capability_count = 2U;
        break;
    case BOOT_STAGE_SHELL:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_SHELL_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_UI_FONT:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_UI_FONT_VERIFIED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_POINTER_DECISION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_KEYBOARD_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_POINTER_OUTCOME:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED;
        descriptor->required_capability_count = 1U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        descriptor->skipped_capabilities[0] =
            BOOT_CAPABILITY_POINTER_INPUT_ABSENT;
        descriptor->skipped_capability_count = 1U;
        break;
    case BOOT_STAGE_UI_LAYOUT:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_UI_FONT_VERIFIED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_UI_LAYOUT_VALIDATED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_EARLY_SCENARIO:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 2U;
        break;
    case BOOT_STAGE_INTERRUPT_PROOFS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 3U;
        break;
    case BOOT_STAGE_TIMER_ROUTING:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_INTERRUPT_ROUTING_PROVED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_TIMER_CALIBRATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_CLOCKS_DISCOVERED;
        descriptor->required_capabilities[4] =
            BOOT_CAPABILITY_INTERRUPT_ROUTING_PROVED;
        descriptor->required_capability_count = 5U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_PCI:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_PCI_RESOURCE_FOUNDATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_DYNAMIC_VECTOR_FOUNDATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_IDT_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_DMA_FOUNDATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_THREADING:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE;
        descriptor->required_capability_count = 2U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_THREADING_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_SCHEDULER:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_THREADING_AVAILABLE;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capability_count = 4U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_SCHEDULER_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_DEVICE_SUBSTRATE_PROOF:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
        descriptor->required_capabilities[4] =
            BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED;
        descriptor->required_capabilities[5] =
            BOOT_CAPABILITY_INTERRUPTS_ENABLED;
        descriptor->required_capabilities[6] =
            BOOT_CAPABILITY_THREADING_AVAILABLE;
        descriptor->required_capabilities[7] =
            BOOT_CAPABILITY_SCHEDULER_AVAILABLE;
        descriptor->required_capabilities[8] =
            BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE;
        descriptor->required_capabilities[9] =
            BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE;
        descriptor->required_capabilities[10] =
            BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE;
        descriptor->required_capability_count = 11U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE;
        descriptor->provided_capability_count = 1U;
        descriptor->skipped_capabilities[0] =
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_FIXTURE_ABSENT;
        descriptor->skipped_capability_count = 1U;
        break;
    case BOOT_STAGE_CLOSING_PROOFS:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_HEAP_AVAILABLE;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE;
        descriptor->required_capabilities[4] =
            BOOT_CAPABILITY_SCHEDULER_AVAILABLE;
        descriptor->required_capability_count = 5U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_DESKTOP_CONSTRUCTION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_UI_FONT_VERIFIED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_UI_LAYOUT_VALIDATED;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED;
        descriptor->required_capability_count = 4U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DESKTOP_SHELL_AVAILABLE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_DESKTOP_ACTIVATION:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_DESKTOP_SHELL_AVAILABLE;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_FRAMEBUFFER_OUTPUT_INSTALLED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
        descriptor->required_capabilities[3] =
            BOOT_CAPABILITY_SURFACE_AVAILABLE;
        descriptor->required_capabilities[4] =
            BOOT_CAPABILITY_UI_FONT_VERIFIED;
        descriptor->required_capabilities[5] =
            BOOT_CAPABILITY_UI_LAYOUT_VALIDATED;
        descriptor->required_capabilities[6] =
            BOOT_CAPABILITY_KEYBOARD_AVAILABLE;
        descriptor->required_capabilities[7] =
            BOOT_CAPABILITY_THREADING_AVAILABLE;
        descriptor->required_capabilities[8] =
            BOOT_CAPABILITY_SCHEDULER_AVAILABLE;
        descriptor->required_capabilities[9] =
            BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE;
        descriptor->required_capability_count = 10U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_FIRST_LIGHT_PROOF:
        descriptor->required_capabilities[0] =
            BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED;
        descriptor->required_capabilities[1] =
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
        descriptor->required_capabilities[2] =
            BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE;
        descriptor->required_capability_count = 3U;
        descriptor->provided_capabilities[0] =
            BOOT_CAPABILITY_FIRST_LIGHT_INSTALLED_PROOF_COMPLETE;
        descriptor->provided_capability_count = 1U;
        break;
    case BOOT_STAGE_INVALID:
    case BOOT_STAGE_COUNT:
    default:
        return false;
    }

    return true;
}

void boot_context_initialize(
    struct boot_context *context,
    uint32_t multiboot_magic,
    uintptr_t multiboot_information_address
)
{
    if (context == NULL) {
        return;
    }

    for (size_t byte = 0U; byte < sizeof(*context); ++byte) {
        ((uint8_t *)context)[byte] = 0U;
    }

    context->multiboot_magic = multiboot_magic;
    context->multiboot_information_address =
        multiboot_information_address;
    context->test_scenario = KERNEL_TEST_NONE;
}

enum boot_ledger_status boot_plan_build(struct boot_ledger *ledger)
{
    enum boot_ledger_status status = BOOT_LEDGER_STATUS_OK;

    if (ledger == NULL) {
        return BOOT_LEDGER_STATUS_NULL_ARGUMENT;
    }

    boot_ledger_reset(ledger);

    for (size_t index = 0U;
         status == BOOT_LEDGER_STATUS_OK &&
            index < sizeof(installed_descriptors) /
                sizeof(installed_descriptors[0]);
        ++index) {
        struct boot_stage_descriptor descriptor = installed_descriptors[index];

        if (!declare_dependencies(&descriptor)) {
            ledger->status = BOOT_LEDGER_STATUS_UNKNOWN_STAGE_IDENTIFIER;
            ledger->refusal_stage = descriptor.id;
            ledger->refusal_capability = BOOT_CAPABILITY_INVALID;
            return ledger->status;
        }

        status = boot_ledger_add_stage(ledger, &descriptor);
    }

    return status;
}

static void synthetic_stage_success(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_succeed(descriptor, result);
}

static void synthetic_pointer_absent(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_skip(descriptor, result);
}

bool boot_plan_pointer_absence_self_test(void)
{
    static struct boot_ledger ledger;
    static struct boot_context context;
    const struct boot_stage_descriptor descriptors[] = {
        {
            .id = BOOT_STAGE_POINTER_DECISION,
            .name = "synthetic pointer decision",
            .provided_capabilities = {
                BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED
            },
            .provided_capability_count = 1U,
            .required = true,
            .phase = BOOT_PHASE_RUNTIME,
            .execute = synthetic_stage_success
        },
        {
            .id = BOOT_STAGE_POINTER_OUTCOME,
            .name = "synthetic pointer absence",
            .required_capabilities = {
                BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED
            },
            .required_capability_count = 1U,
            .provided_capabilities = {
                BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE
            },
            .provided_capability_count = 1U,
            .skipped_capabilities = {
                BOOT_CAPABILITY_POINTER_INPUT_ABSENT
            },
            .skipped_capability_count = 1U,
            .skip_preserves_health = true,
            .phase = BOOT_PHASE_RUNTIME,
            .execute = synthetic_pointer_absent
        },
        {
            .id = BOOT_STAGE_UI_LAYOUT,
            .name = "synthetic keyboard-only layout",
            .required_capabilities = {
                BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED,
                BOOT_CAPABILITY_POINTER_INPUT_ABSENT
            },
            .required_capability_count = 2U,
            .provided_capabilities = {
                BOOT_CAPABILITY_UI_LAYOUT_VALIDATED
            },
            .provided_capability_count = 1U,
            .required = true,
            .phase = BOOT_PHASE_RUNTIME,
            .execute = synthetic_stage_success
        },
        {
            .id = BOOT_STAGE_DESKTOP_CONSTRUCTION,
            .name = "synthetic keyboard-only desktop",
            .required_capabilities = {
                BOOT_CAPABILITY_POINTER_INPUT_ABSENT,
                BOOT_CAPABILITY_UI_LAYOUT_VALIDATED
            },
            .required_capability_count = 2U,
            .provided_capabilities = {
                BOOT_CAPABILITY_DESKTOP_SHELL_AVAILABLE
            },
            .provided_capability_count = 1U,
            .required = true,
            .phase = BOOT_PHASE_PROOFS,
            .execute = synthetic_stage_success
        }
    };
    enum boot_ledger_status status;

    boot_context_initialize(&context, 0U, 0U);
    boot_ledger_reset(&ledger);
    for (size_t index = 0U;
         index < sizeof(descriptors) / sizeof(descriptors[0]); ++index) {
        status = boot_ledger_add_stage(&ledger, &descriptors[index]);
        if (status != BOOT_LEDGER_STATUS_OK) {
            return false;
        }
    }
    status = boot_ledger_validate(&ledger);
    if (status == BOOT_LEDGER_STATUS_OK) {
        status = boot_ledger_execute(&ledger, &context);
    }

    const struct boot_stage_receipt *outcome =
        boot_ledger_receipt_for(&ledger, BOOT_STAGE_POINTER_OUTCOME);
    const struct boot_stage_receipt *desktop =
        boot_ledger_receipt_for(&ledger, BOOT_STAGE_DESKTOP_CONSTRUCTION);

    return status == BOOT_LEDGER_STATUS_OK && ledger.executed &&
        !ledger.degraded && ledger.optional_skip_count == 1U &&
        outcome != NULL && outcome->result == BOOT_RECEIPT_SKIPPED &&
        desktop != NULL && desktop->result == BOOT_RECEIPT_RAN &&
        boot_ledger_has_capability(&ledger,
            BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED) &&
        boot_ledger_has_capability(&ledger,
            BOOT_CAPABILITY_POINTER_INPUT_ABSENT) &&
        !boot_ledger_has_capability(&ledger,
            BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE) &&
        boot_ledger_has_capability(&ledger,
            BOOT_CAPABILITY_DESKTOP_SHELL_AVAILABLE) &&
        boot_ledger_fingerprint_valid(&ledger);
}

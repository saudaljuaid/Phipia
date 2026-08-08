/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>
#include <zenith/console.h>
#include <zenith/memory.h>
#include <zenith/self_test.h>

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
    struct boot_context context;
    struct frame_allocator_stats stats;
    enum boot_status boot_status;
    enum frame_status frame_status;

    console_initialize();
    console_write("Zenith OS: kernel online\n");

    if (!boot_parser_self_test()) {
        console_panic("Multiboot2 parser self-test failed");
    }

    console_write("Zenith OS: parser rejection tests passed\n");

    boot_status = boot_context_parse(magic, boot_information, &context);

    if (boot_status != BOOT_STATUS_OK) {
        console_panic(boot_status_string(boot_status));
    }

    report_boot_context(&context);
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
    console_halt();
}

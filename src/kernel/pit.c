/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/apic.h>
#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/pic.h>
#include <seneri/pit.h>

#define PIT_INPUT_FREQUENCY UINT32_C(1193182)
#define PIT_CHANNEL_ZERO UINT16_C(0x40)
#define PIT_COMMAND UINT16_C(0x43)
#define PIT_CHANNEL_ZERO_MODE_THREE UINT8_C(0x36)
#define PIT_IRQ 0U
#define PIT_VECTOR INTERRUPT_PIC_MASTER_BASE
#define PIT_IOAPIC_VECTOR (INTERRUPT_IOAPIC_BASE + PIT_IRQ)

static volatile uint64_t tick_counter __attribute__((aligned(8)));
static uint32_t configured_frequency;
static bool running;
static enum pit_route active_route;

static void pit_interrupt_handler(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    ++tick_counter;
}

static uint8_t route_vector(enum pit_route route)
{
    return route == PIT_ROUTE_IO_APIC
        ? (uint8_t)PIT_IOAPIC_VECTOR
        : (uint8_t)PIT_VECTOR;
}

/*
 * Unmask the timer at whichever interrupt controller owns it. The 8259 line
 * stays masked on the I/O APIC path, so exactly one controller can deliver
 * IRQ0 at a time and a duplicate tick would be a routing bug, not a race.
 */
static enum pit_status unmask_route(enum pit_route route)
{
    if (route == PIT_ROUTE_IO_APIC) {
        const enum ioapic_status status = ioapic_route_isa_irq(
            (uint8_t)PIT_IRQ,
            (uint8_t)PIT_IOAPIC_VECTOR,
            apic_get_state().id
        );

        return status == IOAPIC_STATUS_OK
            ? PIT_STATUS_OK
            : PIT_STATUS_IOAPIC_FAILURE;
    }

    return pic_set_mask(PIT_IRQ, false) == PIC_STATUS_OK
        ? PIT_STATUS_OK
        : PIT_STATUS_PIC_FAILURE;
}

static enum pit_status mask_route(enum pit_route route)
{
    if (route == PIT_ROUTE_IO_APIC) {
        return ioapic_mask_isa_irq((uint8_t)PIT_IRQ) == IOAPIC_STATUS_OK
            ? PIT_STATUS_OK
            : PIT_STATUS_IOAPIC_FAILURE;
    }

    return pic_set_mask(PIT_IRQ, true) == PIC_STATUS_OK
        ? PIT_STATUS_OK
        : PIT_STATUS_PIC_FAILURE;
}

enum pit_status pit_start(uint32_t frequency_hz, enum pit_route route)
{
    uint32_t divisor;
    uint8_t vector;
    enum interrupt_status interrupt_status;
    enum pit_status status;

    if (running) {
        return PIT_STATUS_ALREADY_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (route != PIT_ROUTE_LEGACY_PIC && route != PIT_ROUTE_IO_APIC) {
        return PIT_STATUS_BAD_ROUTE;
    }

    if (route == PIT_ROUTE_IO_APIC && !ioapic_is_initialized()) {
        return PIT_STATUS_IOAPIC_FAILURE;
    }

    if (frequency_hz == 0U || frequency_hz > PIT_INPUT_FREQUENCY) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    divisor = (PIT_INPUT_FREQUENCY + frequency_hz / 2U) / frequency_hz;

    if (divisor == 0U || divisor > UINT16_MAX) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    vector = route_vector(route);
    interrupt_status = interrupt_register_handler(
        vector,
        pit_interrupt_handler,
        NULL
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return PIT_STATUS_INTERRUPT_FAILURE;
    }

    cpu_out8(PIT_COMMAND, PIT_CHANNEL_ZERO_MODE_THREE);
    cpu_out8(PIT_CHANNEL_ZERO, (uint8_t)(divisor & UINT32_C(0xFF)));
    cpu_out8(PIT_CHANNEL_ZERO, (uint8_t)((divisor >> 8U) & UINT32_C(0xFF)));

    tick_counter = 0U;
    configured_frequency = PIT_INPUT_FREQUENCY / divisor;
    active_route = route;
    running = true;
    status = unmask_route(route);

    if (status != PIT_STATUS_OK) {
        running = false;
        configured_frequency = 0U;
        (void)interrupt_unregister_handler(vector);
        return status;
    }

    return PIT_STATUS_OK;
}

enum pit_route pit_active_route(void)
{
    return active_route;
}

enum pit_status pit_stop(void)
{
    enum pit_status status;
    enum interrupt_status interrupt_status;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    status = mask_route(active_route);

    if (status != PIT_STATUS_OK) {
        return status;
    }

    interrupt_status = interrupt_unregister_handler(route_vector(active_route));

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return PIT_STATUS_INTERRUPT_FAILURE;
    }

    running = false;
    configured_frequency = 0U;
    return PIT_STATUS_OK;
}

uint64_t pit_ticks(void)
{
    return tick_counter;
}

uint32_t pit_frequency(void)
{
    return configured_frequency;
}

bool pit_is_running(void)
{
    return running;
}

enum pit_status pit_wait_for_ticks(uint64_t tick_count)
{
    uint64_t target;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_count == 0U) {
        return PIT_STATUS_OK;
    }

    if (tick_count > UINT64_MAX - tick_counter) {
        target = UINT64_MAX;
    } else {
        target = tick_counter + tick_count;
    }

    while (tick_counter < target) {
        cpu_enable_and_halt();
    }

    cpu_interrupt_disable();
    return PIT_STATUS_OK;
}

const char *pit_status_string(enum pit_status status)
{
    switch (status) {
    case PIT_STATUS_OK:
        return "ok";
    case PIT_STATUS_ALREADY_RUNNING:
        return "PIT was started twice";
    case PIT_STATUS_NOT_RUNNING:
        return "PIT is not running";
    case PIT_STATUS_INTERRUPTS_ENABLED:
        return "PIT mutation requires interrupts disabled";
    case PIT_STATUS_BAD_FREQUENCY:
        return "PIT frequency cannot be represented";
    case PIT_STATUS_INTERRUPT_FAILURE:
        return "PIT interrupt handler operation failed";
    case PIT_STATUS_PIC_FAILURE:
        return "PIT could not update the PIC mask";
    case PIT_STATUS_BAD_ROUTE:
        return "PIT was given an unknown interrupt route";
    case PIT_STATUS_IOAPIC_FAILURE:
        return "PIT could not route its interrupt through the I/O APIC";
    default:
        return "unknown PIT status";
    }
}

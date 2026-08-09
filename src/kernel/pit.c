/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/apic.h>
#include <zenith/cpu.h>
#include <zenith/interrupts.h>
#include <zenith/pit.h>

#define PIT_CHANNEL_ZERO UINT16_C(0x40)
#define PIT_COMMAND UINT16_C(0x43)
#define PIT_CHANNEL_ZERO_MODE_THREE UINT8_C(0x36)
#define PIT_VECTOR APIC_TIMER_VECTOR
#define PIT_WAIT_ATTEMPT_LIMIT UINT64_C(100000000)

static volatile uint64_t tick_counter __attribute__((aligned(8)));
static uint32_t configured_frequency;
static uint32_t configured_divisor;
static bool running;

static void pit_interrupt_handler(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    if (tick_counter != UINT64_MAX) {
        ++tick_counter;
    }
}

enum pit_status pit_start(uint32_t frequency_hz)
{
    uint32_t divisor;
    enum apic_status apic_status;
    enum interrupt_status interrupt_status;

    if (running) {
        return PIT_STATUS_ALREADY_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (frequency_hz == 0U || frequency_hz > PIT_INPUT_FREQUENCY_HZ) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    divisor = (PIT_INPUT_FREQUENCY_HZ + frequency_hz / 2U) /
        frequency_hz;

    if (divisor == 0U || divisor > UINT16_MAX) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    interrupt_status = interrupt_register_handler(
        (uint8_t)PIT_VECTOR,
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
    configured_frequency = PIT_INPUT_FREQUENCY_HZ / divisor;
    configured_divisor = divisor;
    running = true;
    apic_status = apic_timer_set_mask(false);

    if (apic_status != APIC_STATUS_OK) {
        (void)apic_timer_set_mask(true);
        running = false;
        configured_frequency = 0U;
        configured_divisor = 0U;
        (void)interrupt_unregister_handler((uint8_t)PIT_VECTOR);
        return PIT_STATUS_APIC_FAILURE;
    }

    return PIT_STATUS_OK;
}

enum pit_status pit_stop(void)
{
    enum apic_status apic_status;
    enum interrupt_status interrupt_status;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    apic_status = apic_timer_set_mask(true);

    if (apic_status != APIC_STATUS_OK) {
        return PIT_STATUS_APIC_FAILURE;
    }

    interrupt_status = interrupt_unregister_handler((uint8_t)PIT_VECTOR);

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return PIT_STATUS_INTERRUPT_FAILURE;
    }

    running = false;
    configured_frequency = 0U;
    configured_divisor = 0U;
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

uint32_t pit_divisor(void)
{
    return configured_divisor;
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
        return PIT_STATUS_TICK_OVERFLOW;
    }

    target = tick_counter + tick_count;
    cpu_interrupt_enable();

    for (uint64_t attempt = 0U;
         attempt < PIT_WAIT_ATTEMPT_LIMIT;
         ++attempt) {
        if (tick_counter >= target) {
            cpu_interrupt_disable();
            return PIT_STATUS_OK;
        }

        __asm__ volatile ("pause" : : : "memory");
    }

    cpu_interrupt_disable();
    return PIT_STATUS_TIMEOUT;
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
    case PIT_STATUS_APIC_FAILURE:
        return "PIT could not update the I/O APIC route";
    case PIT_STATUS_TICK_OVERFLOW:
        return "PIT tick target overflows";
    case PIT_STATUS_TIMEOUT:
        return "PIT interrupt wait exceeded its bounded attempt limit";
    default:
        return "unknown PIT status";
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_TEST_H
#define SENERI_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include <seneri/boot.h>
#include <seneri/interrupts.h>

enum kernel_test_scenario {
    KERNEL_TEST_NONE = 0,
    KERNEL_TEST_NORMAL,
    KERNEL_TEST_BREAKPOINT,
    KERNEL_TEST_INVALID_OPCODE,
    KERNEL_TEST_PAGE_FAULT,
    KERNEL_TEST_IST,
    KERNEL_TEST_PIT,
    KERNEL_TEST_UNEXPECTED,
    KERNEL_TEST_DOUBLE_FAULT,
    KERNEL_TEST_APIC,
    KERNEL_TEST_IOAPIC,
    KERNEL_TEST_RETIRED,
    KERNEL_TEST_APIC_TIMER,
    KERNEL_TEST_TSC,
    KERNEL_TEST_INVALID
};

enum kernel_test_scenario kernel_test_select(const struct boot_context *context);
void kernel_test_run(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_complete_normal(void);
bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame);
const char *kernel_test_scenario_name(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_fail(const char *reason);

extern volatile uint8_t kernel_test_double_fault_armed;

#endif

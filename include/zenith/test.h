/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_TEST_H
#define ZENITH_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenith/boot.h>
#include <zenith/interrupts.h>

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
    KERNEL_TEST_WRITE_PROTECT,
    KERNEL_TEST_NX,
    KERNEL_TEST_APIC,
    KERNEL_TEST_LAPIC_TIMER,
    KERNEL_TEST_HEAP,
    KERNEL_TEST_SCHEDULER,
    KERNEL_TEST_SCHEDULER_GUARD,
    KERNEL_TEST_INVALID
};

enum kernel_test_scenario kernel_test_select(const struct boot_context *context);
void kernel_test_run(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_complete_normal(void);
_Noreturn void kernel_test_complete_lapic_timer(void);
_Noreturn void kernel_test_complete_heap(void);
_Noreturn void kernel_test_complete_scheduler(void);
_Noreturn void kernel_test_complete_scheduler_guard(void);
void kernel_test_scheduler_normal_proof(void);
bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame);
const char *kernel_test_scenario_name(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_fail(const char *reason);

bool frame_test_fail_allocate_after(size_t successful_allocations);
bool frame_test_report_out_of_memory_after(size_t successful_allocations);
bool apic_test_discard_calibration_attempts(size_t attempt_count);
bool virtual_memory_test_fail_heap_map_after(size_t successful_maps);
bool virtual_memory_test_report_uncertain_heap_map_once(void);
bool virtual_memory_test_fail_task_stack_map_after(size_t successful_maps);
bool virtual_memory_test_report_uncertain_task_stack_map_once(void);
bool scheduler_test_callee_saved_probe(void);
_Noreturn void scheduler_test_trigger_guard(uintptr_t address);

extern const uint8_t scheduler_guard_fault_site[];

extern volatile uint8_t kernel_test_double_fault_armed;

#endif

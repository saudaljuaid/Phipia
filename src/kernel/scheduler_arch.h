/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_ARCH_H
#define ZENITH_SCHEDULER_ARCH_H

#include <stdbool.h>
#include <stdint.h>

_Noreturn void scheduler_task_first_entry(void);
_Noreturn void scheduler_task_return_trampoline(void);
_Noreturn void scheduler_task_first_entry_c(void);
bool scheduler_test_callee_saved_probe(void);
bool scheduler_test_timer_register_probe(void);
bool scheduler_test_interrupt_while_locked(void);
void scheduler_test_xstate_clobber(void);
_Noreturn void scheduler_test_trigger_guard(uintptr_t address);
_Noreturn void scheduler_test_trigger_nm(void);

extern const uint8_t scheduler_task_first_entry_address[];
extern const uint8_t scheduler_task_return_trampoline_address[];
extern const uint8_t scheduler_guard_fault_site[];
extern const uint8_t scheduler_nm_fault_site[];

#endif

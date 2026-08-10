/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_SCHEDULER_ARCH_H
#define ZENITH_SCHEDULER_ARCH_H

#include <stdbool.h>
#include <stdint.h>

struct scheduler_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uintptr_t rsp;
    uintptr_t rip;
};

void scheduler_context_switch(
    struct scheduler_context *previous,
    const struct scheduler_context *next
);
_Noreturn void scheduler_task_first_entry(void);
_Noreturn void scheduler_task_return_trampoline(void);
_Noreturn void scheduler_task_first_entry_c(void);
bool scheduler_test_callee_saved_probe(void);
_Noreturn void scheduler_test_trigger_guard(uintptr_t address);

extern const uint8_t scheduler_context_switch_resume[];
extern const uint8_t scheduler_task_first_entry_address[];
extern const uint8_t scheduler_task_return_trampoline_address[];
extern const uint8_t scheduler_guard_fault_site[];

#endif

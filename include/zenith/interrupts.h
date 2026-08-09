/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ZENITH_INTERRUPTS_H
#define ZENITH_INTERRUPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTERRUPT_VECTOR_COUNT 256U
#define INTERRUPT_EXCEPTION_COUNT 32U
#define INTERRUPT_PIC_MASTER_BASE 32U
#define INTERRUPT_PIC_SLAVE_BASE 40U
#define INTERRUPT_PIC_LIMIT 48U
#define INTERRUPT_IST_TEST_VECTOR UINT8_C(0xF0)

struct interrupt_frame {
    uint64_t cr2;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*interrupt_handler_t)(
    struct interrupt_frame *frame,
    void *context
);

enum interrupt_status {
    INTERRUPT_STATUS_OK = 0,
    INTERRUPT_STATUS_ALREADY_INITIALIZED,
    INTERRUPT_STATUS_NOT_INITIALIZED,
    INTERRUPT_STATUS_CPU_TABLE_FAILURE,
    INTERRUPT_STATUS_PIC_FAILURE,
    INTERRUPT_STATUS_BAD_IDT,
    INTERRUPT_STATUS_INTERRUPTS_ENABLED,
    INTERRUPT_STATUS_HANDLER_PRESENT,
    INTERRUPT_STATUS_HANDLER_MISSING,
    INTERRUPT_STATUS_RESERVED_VECTOR,
    INTERRUPT_STATUS_NULL_HANDLER,
    INTERRUPT_STATUS_BAD_ARGUMENT
};

enum interrupt_status interrupts_initialize(void);
enum interrupt_status interrupts_validate(void);
bool interrupts_ready(void);
enum interrupt_status interrupt_register_handler(
    uint8_t vector,
    interrupt_handler_t handler,
    void *context
);
enum interrupt_status interrupt_unregister_handler(uint8_t vector);
void interrupt_dispatch(struct interrupt_frame *frame);
const char *interrupt_status_string(enum interrupt_status status);
const char *interrupt_exception_name(uint8_t vector);
void interrupt_test_set_gate_present(uint8_t vector, bool present);

bool interrupt_breakpoint_self_test(void);
bool interrupt_ist_self_test(void);
bool interrupt_pic_spurious_self_test(void);
bool interrupt_frame_layout_self_test(void);
bool interrupt_trigger_register_probe(void);
void interrupt_trigger_ist_probe(void);
void interrupt_trigger_spurious_irq7(void);
void interrupt_trigger_spurious_irq15(void);
void interrupt_trigger_apic_spurious(void);
_Noreturn void interrupt_trigger_invalid_opcode(void);
_Noreturn void interrupt_trigger_page_fault(void);
_Noreturn void interrupt_trigger_write_protect(void);
_Noreturn void interrupt_trigger_nx(void);
_Noreturn void interrupt_trigger_unexpected(void);

extern const uintptr_t interrupt_vector_table[INTERRUPT_VECTOR_COUNT];
extern const uint8_t interrupt_vector_table_end[];
extern const uint8_t interrupt_invalid_opcode_site[];
extern const uint8_t interrupt_page_fault_site[];
extern const uint8_t interrupt_write_protect_site[];
extern const uint8_t interrupt_write_protect_target[];
extern const uint8_t interrupt_non_executable_target[];

#endif

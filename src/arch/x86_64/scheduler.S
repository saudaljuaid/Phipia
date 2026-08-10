/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded cooperative task context switching and scheduler fault probes. */

.set CONTEXT_RBX, 0
.set CONTEXT_RBP, 8
.set CONTEXT_R12, 16
.set CONTEXT_R13, 24
.set CONTEXT_R14, 32
.set CONTEXT_R15, 40
.set CONTEXT_RSP, 48
.set CONTEXT_RIP, 56
.set SCHEDULER_STATUS_OK, 0

.section .text, "ax"
.code64
.extern scheduler_task_first_entry_c
.extern scheduler_task_exit
.extern scheduler_yield

.global scheduler_context_switch
.type scheduler_context_switch, @function
scheduler_context_switch:
    movq %rbx, CONTEXT_RBX(%rdi)
    movq %rbp, CONTEXT_RBP(%rdi)
    movq %r12, CONTEXT_R12(%rdi)
    movq %r13, CONTEXT_R13(%rdi)
    movq %r14, CONTEXT_R14(%rdi)
    movq %r15, CONTEXT_R15(%rdi)
    movq %rsp, CONTEXT_RSP(%rdi)
    leaq scheduler_context_switch_resume(%rip), %rax
    movq %rax, CONTEXT_RIP(%rdi)

    movq CONTEXT_RBX(%rsi), %rbx
    movq CONTEXT_RBP(%rsi), %rbp
    movq CONTEXT_R12(%rsi), %r12
    movq CONTEXT_R13(%rsi), %r13
    movq CONTEXT_R14(%rsi), %r14
    movq CONTEXT_R15(%rsi), %r15
    movq CONTEXT_RSP(%rsi), %rsp
    jmp *CONTEXT_RIP(%rsi)
.global scheduler_context_switch_resume
scheduler_context_switch_resume:
    ret
.size scheduler_context_switch, . - scheduler_context_switch

.global scheduler_task_first_entry
.global scheduler_task_first_entry_address
.type scheduler_task_first_entry, @function
scheduler_task_first_entry:
scheduler_task_first_entry_address:
    cld
    subq $8, %rsp
    call scheduler_task_first_entry_c
    addq $8, %rsp
    jmp scheduler_task_return_trampoline
.size scheduler_task_first_entry, . - scheduler_task_first_entry

.global scheduler_task_return_trampoline
.global scheduler_task_return_trampoline_address
.type scheduler_task_return_trampoline, @function
scheduler_task_return_trampoline:
scheduler_task_return_trampoline_address:
    cld
    subq $8, %rsp
    call scheduler_task_exit
    ud2
.size scheduler_task_return_trampoline, . - scheduler_task_return_trampoline

.global scheduler_test_callee_saved_probe
.type scheduler_test_callee_saved_probe, @function
scheduler_test_callee_saved_probe:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp

    movabsq $0x1122334455667788, %rbx
    movabsq $0x8877665544332211, %rbp
    movabsq $0x1020304050607080, %r12
    movabsq $0x8070605040302010, %r13
    movabsq $0xA1B2C3D4E5F60718, %r14
    movabsq $0x81706F5E4D3C2B1A, %r15

    call scheduler_yield
    cmpl $SCHEDULER_STATUS_OK, %eax
    jne .callee_saved_failed
    call scheduler_yield
    cmpl $SCHEDULER_STATUS_OK, %eax
    jne .callee_saved_failed

    movabsq $0x1122334455667788, %rax
    cmpq %rax, %rbx
    jne .callee_saved_failed
    movabsq $0x8877665544332211, %rax
    cmpq %rax, %rbp
    jne .callee_saved_failed
    movabsq $0x1020304050607080, %rax
    cmpq %rax, %r12
    jne .callee_saved_failed
    movabsq $0x8070605040302010, %rax
    cmpq %rax, %r13
    jne .callee_saved_failed
    movabsq $0xA1B2C3D4E5F60718, %rax
    cmpq %rax, %r14
    jne .callee_saved_failed
    movabsq $0x81706F5E4D3C2B1A, %rax
    cmpq %rax, %r15
    jne .callee_saved_failed
    movl $1, %eax
    jmp .callee_saved_restore
.callee_saved_failed:
    xorl %eax, %eax
.callee_saved_restore:
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size scheduler_test_callee_saved_probe, . - scheduler_test_callee_saved_probe

.global scheduler_test_trigger_guard
.type scheduler_test_trigger_guard, @function
scheduler_test_trigger_guard:
.global scheduler_guard_fault_site
scheduler_guard_fault_site:
    movq (%rdi), %rax
    cli
.guard_fault_returned:
    hlt
    jmp .guard_fault_returned
.size scheduler_test_trigger_guard, . - scheduler_test_trigger_guard

.section .note.GNU-stack, "", @progbits

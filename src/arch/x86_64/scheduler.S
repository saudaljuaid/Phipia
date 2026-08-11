/* SPDX-License-Identifier: GPL-3.0-only */
/* Trap-frame task entry and scheduler fault/preemption probes. */

.set SCHEDULER_STATUS_OK, 0

.section .text, "ax"
.code64
.extern scheduler_task_first_entry_c
.extern scheduler_task_exit
.extern scheduler_yield
.extern scheduler_preempt_stage

.global scheduler_task_first_entry
.global scheduler_task_first_entry_address
.type scheduler_task_first_entry, @function
scheduler_task_first_entry:
scheduler_task_first_entry_address:
    cld
    call scheduler_task_first_entry_c
    jmp scheduler_task_return_trampoline
.size scheduler_task_first_entry, . - scheduler_task_first_entry

.global scheduler_task_return_trampoline
.global scheduler_task_return_trampoline_address
.type scheduler_task_return_trampoline, @function
scheduler_task_return_trampoline:
scheduler_task_return_trampoline_address:
    cld
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

.global scheduler_test_timer_register_probe
.type scheduler_test_timer_register_probe, @function
scheduler_test_timer_register_probe:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $16, %rsp
    movq $100000000, (%rsp)

    movl $0x01010101, %eax
    movl $0x02020202, %ebx
    movl $0x03030303, %ecx
    movl $0x04040404, %edx
    movl $0x05050505, %ebp
    movl $0x06060606, %edi
    movl $0x07070707, %esi
    movl $0x08080808, %r8d
    movl $0x09090909, %r9d
    movl $0x10101010, %r10d
    movl $0x11111111, %r11d
    movl $0x12121212, %r12d
    movl $0x13131313, %r13d
    movl $0x14141414, %r14d
    movl $0x15151515, %r15d
    movq %rax, %xmm0
    movq %rbx, %xmm7
    movq %r15, %xmm15
    std
    movl $1, scheduler_preempt_stage(%rip)
.Ltimer_probe_wait:
    cmpl $2, scheduler_preempt_stage(%rip)
    je .Ltimer_probe_check
    subq $1, (%rsp)
    jz .Ltimer_probe_failed
    pause
    jmp .Ltimer_probe_wait
.Ltimer_probe_check:
    cmpq $0x01010101, %rax
    jne .Ltimer_probe_failed
    cmpq $0x02020202, %rbx
    jne .Ltimer_probe_failed
    cmpq $0x03030303, %rcx
    jne .Ltimer_probe_failed
    cmpq $0x04040404, %rdx
    jne .Ltimer_probe_failed
    cmpq $0x05050505, %rbp
    jne .Ltimer_probe_failed
    cmpq $0x06060606, %rdi
    jne .Ltimer_probe_failed
    cmpq $0x07070707, %rsi
    jne .Ltimer_probe_failed
    cmpq $0x08080808, %r8
    jne .Ltimer_probe_failed
    cmpq $0x09090909, %r9
    jne .Ltimer_probe_failed
    cmpq $0x10101010, %r10
    jne .Ltimer_probe_failed
    cmpq $0x11111111, %r11
    jne .Ltimer_probe_failed
    cmpq $0x12121212, %r12
    jne .Ltimer_probe_failed
    cmpq $0x13131313, %r13
    jne .Ltimer_probe_failed
    cmpq $0x14141414, %r14
    jne .Ltimer_probe_failed
    cmpq $0x15151515, %r15
    jne .Ltimer_probe_failed
    movq %xmm0, %rdx
    cmpq $0x01010101, %rdx
    jne .Ltimer_probe_failed
    movq %xmm7, %rdx
    cmpq $0x02020202, %rdx
    jne .Ltimer_probe_failed
    movq %xmm15, %rdx
    cmpq $0x15151515, %rdx
    jne .Ltimer_probe_failed
    pushfq
    popq %rax
    andq $0x600, %rax
    cmpq $0x600, %rax
    jne .Ltimer_probe_failed
    cld
    movl $0, scheduler_preempt_stage(%rip)
    movl $1, %eax
    jmp .Ltimer_probe_restore
.Ltimer_probe_failed:
    cld
    movl $0, scheduler_preempt_stage(%rip)
    movl $0, %eax
.Ltimer_probe_restore:
    addq $16, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size scheduler_test_timer_register_probe, . - scheduler_test_timer_register_probe

.global scheduler_test_xstate_clobber
.type scheduler_test_xstate_clobber, @function
scheduler_test_xstate_clobber:
    pxor %xmm0, %xmm0
    pcmpeqb %xmm7, %xmm7
    movabsq $0xDEADBEEFCAFEBABE, %rax
    movq %rax, %xmm15
    ret
.size scheduler_test_xstate_clobber, . - scheduler_test_xstate_clobber

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

.global scheduler_test_trigger_nm
.type scheduler_test_trigger_nm, @function
scheduler_test_trigger_nm:
    movq %cr0, %rax
    orq $0x8, %rax
    movq %rax, %cr0
.global scheduler_nm_fault_site
scheduler_nm_fault_site:
    pxor %xmm0, %xmm0
    cli
.nm_fault_returned:
    hlt
    jmp .nm_fault_returned
.size scheduler_test_trigger_nm, . - scheduler_test_trigger_nm

.section .note.GNU-stack, "", @progbits

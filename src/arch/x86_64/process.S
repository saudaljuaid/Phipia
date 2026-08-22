/* SPDX-License-Identifier: GPL-3.0-only */
/* One reviewed IRETQ boundary for the bounded v0.7.0 CPL3 proof. */

/* Mirrors include/sapote/cpu.h and docs/CPL3_INTERRUPT_BOUNDARY.md. */
.set USER_DATA_SELECTOR, 0x2B
.set USER_CODE_SELECTOR, 0x33
.set USER_RFLAGS, 0x2

.section .text, "ax"
.code64

.global process_enter_user
.type process_enter_user, @function
process_enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, process_resume_stack(%rip)
    movb $1, process_boundary_active(%rip)

    movq %rdi, %r10
    movq %rsi, %r11
    pushq $USER_DATA_SELECTOR
    pushq %r11
    pushq $USER_RFLAGS
    pushq $USER_CODE_SELECTOR
    pushq %r10

    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq
    ud2
.size process_enter_user, . - process_enter_user

.global process_user_resume_from_interrupt
.type process_user_resume_from_interrupt, @function
process_user_resume_from_interrupt:
    movb $0, process_boundary_active(%rip)
    movq $0, process_resume_stack(%rip)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size process_user_resume_from_interrupt, . - process_user_resume_from_interrupt

.global process_user_resume_stack
.type process_user_resume_stack, @function
process_user_resume_stack:
    cmpb $0, process_boundary_active(%rip)
    je .resume_stack_inactive
    movq process_resume_stack(%rip), %rax
    ret
.resume_stack_inactive:
    xorl %eax, %eax
    ret
.size process_user_resume_stack, . - process_user_resume_stack

.global process_user_boundary_active
.type process_user_boundary_active, @function
process_user_boundary_active:
    xorl %eax, %eax
    cmpb $0, process_boundary_active(%rip)
    setne %al
    ret
.size process_user_boundary_active, . - process_user_boundary_active

.section .bss, "aw", @nobits
.balign 8
process_resume_stack:
    .skip 8
process_boundary_active:
    .skip 1

.section .note.GNU-stack, "", @progbits

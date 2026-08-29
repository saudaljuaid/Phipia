/* SPDX-License-Identifier: GPL-3.0-only */
.section .text.start, "ax", @progbits
.global _start
.type _start, @function
.extern main
.extern sapote_runtime_initialize
.weak sapote_application_entry_probe
_start:
    movabsq $sapote_application_entry_probe, %rax
    testq %rax, %rax
    jz .Lruntime_start
    call *%rax
.Lruntime_start:
    xorq %rbp, %rbp
    andq $-16, %rsp
    subq $8, %rsp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    call sapote_runtime_initialize
    popq %rdx
    popq %rsi
    popq %rdi
    addq $8, %rsp
    call main
    movslq %eax, %rdi
    movq $1, %rax
    syscall
    ud2
.size _start, . - _start
.section .note.GNU-stack, "", @progbits

/* SPDX-License-Identifier: GPL-3.0-only */
/* Privileged eager-XSAVE helpers.  C callers are built with all SIMD disabled. */

.section .text, "ax"
.code64

.global xstate_arch_read_cr4
.type xstate_arch_read_cr4, @function
xstate_arch_read_cr4:
    movq %cr4, %rax
    ret
.size xstate_arch_read_cr4, . - xstate_arch_read_cr4

.global xstate_arch_write_cr4
.type xstate_arch_write_cr4, @function
xstate_arch_write_cr4:
    movq %rdi, %cr4
    ret
.size xstate_arch_write_cr4, . - xstate_arch_write_cr4

.global xstate_arch_xgetbv
.type xstate_arch_xgetbv, @function
xstate_arch_xgetbv:
    movl %edi, %ecx
    xgetbv
    shlq $32, %rdx
    orq %rdx, %rax
    ret
.size xstate_arch_xgetbv, . - xstate_arch_xgetbv

.global xstate_arch_xsetbv
.type xstate_arch_xsetbv, @function
xstate_arch_xsetbv:
    movl %edi, %ecx
    movq %rsi, %rax
    movq %rsi, %rdx
    shrq $32, %rdx
    xsetbv
    ret
.size xstate_arch_xsetbv, . - xstate_arch_xsetbv

.global xstate_arch_save
.type xstate_arch_save, @function
xstate_arch_save:
    movq %rsi, %rax
    movq %rsi, %rdx
    shrq $32, %rdx
    xsave64 (%rdi)
    ret
.size xstate_arch_save, . - xstate_arch_save

.global xstate_arch_restore
.type xstate_arch_restore, @function
xstate_arch_restore:
    movq %rsi, %rax
    movq %rsi, %rdx
    shrq $32, %rdx
    xrstor64 (%rdi)
    ret
.size xstate_arch_restore, . - xstate_arch_restore

.section .note.GNU-stack, "", @progbits

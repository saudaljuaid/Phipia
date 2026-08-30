/* SPDX-License-Identifier: GPL-3.0-only */
.section .text, "ax", @progbits
.macro SAPOTE_STUB name, count
.global \name
.type \name, @function
\name:
    movq %rdi, %rax
.if \count > 0
    movq %rsi, %rdi
.endif
.if \count > 1
    movq %rdx, %rsi
.endif
.if \count > 2
    movq %rcx, %rdx
.endif
.if \count > 3
    movq %r8, %r10
.endif
.if \count > 4
    movq %r9, %r8
.endif
.if \count > 5
    movq 8(%rsp), %r9
.endif
    syscall
    ret
.size \name, . - \name
.endm
SAPOTE_STUB sapote_syscall0, 0
SAPOTE_STUB sapote_syscall1, 1
SAPOTE_STUB sapote_syscall2, 2
SAPOTE_STUB sapote_syscall3, 3
SAPOTE_STUB sapote_syscall4, 4
SAPOTE_STUB sapote_syscall5, 5
SAPOTE_STUB sapote_syscall6, 6
.section .note.GNU-stack, "", @progbits

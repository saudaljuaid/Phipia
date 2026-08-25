/* SPDX-License-Identifier: GPL-3.0-only */
/* Hidden freestanding memory primitives used by C and Rust code generation. */

.section .text, "ax"
.code64

.global memcpy
.hidden memcpy
.type memcpy, @function
memcpy:
    movq %rdi, %rax
    movq %rdx, %rcx
    cld
    rep movsb
    ret
.size memcpy, . - memcpy

.global memset
.hidden memset
.type memset, @function
memset:
    movq %rdi, %r8
    movq %rdx, %rcx
    movl %esi, %eax
    cld
    rep stosb
    movq %r8, %rax
    ret
.size memset, . - memset

.global memcmp
.hidden memcmp
.type memcmp, @function
memcmp:
    testq %rdx, %rdx
    jz .Lmemcmp_equal
.Lmemcmp_next:
    movzbl (%rdi), %eax
    movzbl (%rsi), %ecx
    cmpl %ecx, %eax
    jne .Lmemcmp_different
    incq %rdi
    incq %rsi
    decq %rdx
    jnz .Lmemcmp_next
.Lmemcmp_equal:
    xorl %eax, %eax
    ret
.Lmemcmp_different:
    subl %ecx, %eax
    ret
.size memcmp, . - memcmp

.section .note.GNU-stack, "", @progbits

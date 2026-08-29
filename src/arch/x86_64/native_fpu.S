/* SPDX-License-Identifier: GPL-3.0-only */
/* The only kernel translation unit permitted to touch x87/SIMD state. */

.section .text, "ax"
.code64

.global native_fxsave
.type native_fxsave, @function
native_fxsave:
    fxsave64 (%rdi)
    ret
.size native_fxsave, . - native_fxsave

.global native_fxrstor
.type native_fxrstor, @function
native_fxrstor:
    fxrstor64 (%rdi)
    ret
.size native_fxrstor, . - native_fxrstor

.global native_fxinit
.type native_fxinit, @function
native_fxinit:
    fninit
    subq $16, %rsp
    movl $0x1F80, (%rsp)
    ldmxcsr (%rsp)
    pxor %xmm0, %xmm0
    pxor %xmm1, %xmm1
    pxor %xmm2, %xmm2
    pxor %xmm3, %xmm3
    pxor %xmm4, %xmm4
    pxor %xmm5, %xmm5
    pxor %xmm6, %xmm6
    pxor %xmm7, %xmm7
    pxor %xmm8, %xmm8
    pxor %xmm9, %xmm9
    pxor %xmm10, %xmm10
    pxor %xmm11, %xmm11
    pxor %xmm12, %xmm12
    pxor %xmm13, %xmm13
    pxor %xmm14, %xmm14
    pxor %xmm15, %xmm15
    addq $16, %rsp
    fxsave64 (%rdi)
    ret
.size native_fxinit, . - native_fxinit

.section .note.GNU-stack, "", @progbits

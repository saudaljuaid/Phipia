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
    addq $16, %rsp
    fxsave64 (%rdi)
    ret
.size native_fxinit, . - native_fxinit

.section .note.GNU-stack, "", @progbits

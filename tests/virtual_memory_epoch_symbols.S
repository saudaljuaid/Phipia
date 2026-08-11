/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef _WIN32
    .section .vmtest_kernel,"dw"
#else
    .section .vmtest_kernel,"aw",@progbits
#endif
    .p2align 12
    .global __kernel_start
__kernel_start:
    .space 4096
    .global __text_start
__text_start:
    .space 4096
    .global __text_end
    .global __rodata_start
__text_end:
__rodata_start:
    .space 4096
    .global __rodata_end
    .global __data_start
__rodata_end:
__data_start:
    .space 4096
    .global __data_end
    .global __bss_start
__data_end:
__bss_start:
    .space 4096
    .global __bss_end
    .global __kernel_end
__bss_end:
__kernel_end:
#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif

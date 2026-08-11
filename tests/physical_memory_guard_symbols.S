/* SPDX-License-Identifier: GPL-3.0-only */
.globl __kernel_start
.set __kernel_start, 0x00200000

.globl __kernel_end
.set __kernel_end, 0x00240000

#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif

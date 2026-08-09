/* SPDX-License-Identifier: GPL-3.0-only */
/* Small, auditable x86_64 primitives that cannot be expressed portably in C. */

.section .text, "ax"
.code64

.global cpu_interrupt_disable
.type cpu_interrupt_disable, @function
cpu_interrupt_disable:
    cli
    ret
.size cpu_interrupt_disable, . - cpu_interrupt_disable

.global cpu_interrupt_enable
.type cpu_interrupt_enable, @function
cpu_interrupt_enable:
    sti
    ret
.size cpu_interrupt_enable, . - cpu_interrupt_enable

.global cpu_interrupts_enabled
.type cpu_interrupts_enabled, @function
cpu_interrupts_enabled:
    pushfq
    popq %rax
    shrq $9, %rax
    andl $1, %eax
    ret
.size cpu_interrupts_enabled, . - cpu_interrupts_enabled

.global cpu_halt_once
.type cpu_halt_once, @function
cpu_halt_once:
    hlt
    ret
.size cpu_halt_once, . - cpu_halt_once

.global cpu_enable_and_halt
.type cpu_enable_and_halt, @function
cpu_enable_and_halt:
    sti
    hlt
    ret
.size cpu_enable_and_halt, . - cpu_enable_and_halt

.global cpu_in8
.type cpu_in8, @function
cpu_in8:
    movw %di, %dx
    inb %dx, %al
    ret
.size cpu_in8, . - cpu_in8

.global cpu_out8
.type cpu_out8, @function
cpu_out8:
    movw %di, %dx
    movb %sil, %al
    outb %al, %dx
    ret
.size cpu_out8, . - cpu_out8

.global cpu_out32
.type cpu_out32, @function
cpu_out32:
    movw %di, %dx
    movl %esi, %eax
    outl %eax, %dx
    ret
.size cpu_out32, . - cpu_out32

.global cpu_io_wait
.type cpu_io_wait, @function
cpu_io_wait:
    xorl %eax, %eax
    outb %al, $0x80
    ret
.size cpu_io_wait, . - cpu_io_wait

.global cpu_read_cr2
.type cpu_read_cr2, @function
cpu_read_cr2:
    movq %cr2, %rax
    ret
.size cpu_read_cr2, . - cpu_read_cr2

.global cpu_read_cr0
.type cpu_read_cr0, @function
cpu_read_cr0:
    movq %cr0, %rax
    ret
.size cpu_read_cr0, . - cpu_read_cr0

.global cpu_read_cr3
.type cpu_read_cr3, @function
cpu_read_cr3:
    movq %cr3, %rax
    ret
.size cpu_read_cr3, . - cpu_read_cr3

.global cpu_write_cr0
.type cpu_write_cr0, @function
cpu_write_cr0:
    movq %rdi, %cr0
    ret
.size cpu_write_cr0, . - cpu_write_cr0

.global cpu_write_cr3
.type cpu_write_cr3, @function
cpu_write_cr3:
    movq %rdi, %cr3
    ret
.size cpu_write_cr3, . - cpu_write_cr3

.global cpu_invalidate_page
.type cpu_invalidate_page, @function
cpu_invalidate_page:
    invlpg (%rdi)
    ret
.size cpu_invalidate_page, . - cpu_invalidate_page

.global cpu_read_msr
.type cpu_read_msr, @function
cpu_read_msr:
    movl %edi, %ecx
    rdmsr
    shlq $32, %rdx
    orq %rdx, %rax
    ret
.size cpu_read_msr, . - cpu_read_msr

.global cpu_write_msr
.type cpu_write_msr, @function
cpu_write_msr:
    movl %edi, %ecx
    movl %esi, %eax
    movq %rsi, %rdx
    shrq $32, %rdx
    wrmsr
    ret
.size cpu_write_msr, . - cpu_write_msr

.global cpu_cpuid
.type cpu_cpuid, @function
cpu_cpuid:
    pushq %rbx
    movq %rdx, %r8
    movl %edi, %eax
    movl %esi, %ecx
    cpuid
    movl %eax, 0(%r8)
    movl %ebx, 4(%r8)
    movl %ecx, 8(%r8)
    movl %edx, 12(%r8)
    popq %rbx
    ret
.size cpu_cpuid, . - cpu_cpuid

.global cpu_write_back_and_invalidate
.type cpu_write_back_and_invalidate, @function
cpu_write_back_and_invalidate:
    wbinvd
    ret
.size cpu_write_back_and_invalidate, . - cpu_write_back_and_invalidate

.global cpu_read_cs
.type cpu_read_cs, @function
cpu_read_cs:
    xorl %eax, %eax
    movw %cs, %ax
    ret
.size cpu_read_cs, . - cpu_read_cs

.global cpu_read_task_register
.type cpu_read_task_register, @function
cpu_read_task_register:
    xorl %eax, %eax
    strw %ax
    ret
.size cpu_read_task_register, . - cpu_read_task_register

.global cpu_read_stack_pointer
.type cpu_read_stack_pointer, @function
cpu_read_stack_pointer:
    movq %rsp, %rax
    ret
.size cpu_read_stack_pointer, . - cpu_read_stack_pointer

.global cpu_load_gdt_and_tss
.type cpu_load_gdt_and_tss, @function
cpu_load_gdt_and_tss:
    lgdtq (%rdi)
    pushq $0x08
    leaq .segments_reloaded(%rip), %rax
    pushq %rax
    lretq
.segments_reloaded:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movw $0x18, %ax
    ltrw %ax
    ret
.size cpu_load_gdt_and_tss, . - cpu_load_gdt_and_tss

.global cpu_load_idt
.type cpu_load_idt, @function
cpu_load_idt:
    lidtq (%rdi)
    ret
.size cpu_load_idt, . - cpu_load_idt

.global cpu_store_gdt
.type cpu_store_gdt, @function
cpu_store_gdt:
    sgdtq (%rdi)
    ret
.size cpu_store_gdt, . - cpu_store_gdt

.global cpu_store_idt
.type cpu_store_idt, @function
cpu_store_idt:
    sidtq (%rdi)
    ret
.size cpu_store_idt, . - cpu_store_idt

.section .note.GNU-stack, "", @progbits

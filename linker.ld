/* SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_BASE = 1M;

PHDRS
{
    multiboot PT_LOAD FLAGS(4);
    text      PT_LOAD FLAGS(5);
    rodata    PT_LOAD FLAGS(4);
    data      PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_BASE;
    __kernel_start = .;

    .multiboot ALIGN(8) :
    {
        __multiboot_header_start = .;
        KEEP(*(.multiboot))
        __multiboot_header_end = .;
    } :multiboot

    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.boot.text)
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4K);
        __data_end = .;
    } :data

    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
        __bss_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }

    ASSERT(__multiboot_header_end - __multiboot_header_start >= 24,
           "Multiboot2 header is incomplete")
    ASSERT(__multiboot_header_start - KERNEL_BASE < 32768,
           "Multiboot2 header is outside the first 32 KiB")
    ASSERT(__kernel_end < 0x40000000,
           "Day-one identity map no longer covers the kernel")
    ASSERT(__kernel_end - __kernel_start <= 64M,
           "Permanent page-table policy supports a kernel up to 64 MiB")
    ASSERT(interrupt_vector_table_end - interrupt_vector_table == 256 * 8,
           "Interrupt vector table must contain exactly 256 addresses")
}

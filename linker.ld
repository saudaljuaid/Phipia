/* SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_BASE = 1M;

/*
 * src/kernel/paging.c maps this image one 4 KiB page at a time and gives each
 * section its own permissions, so no two permissions may share a page. Every
 * section therefore starts on a page boundary and pads to one, which makes each
 * __*_start and __*_end pair a whole number of pages and makes the end of one
 * section exactly the start of the next.
 */
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
    ASSERT(interrupt_vector_table_end - interrupt_vector_table == 256 * 8,
           "Interrupt vector table must contain exactly 256 addresses")

    /* Each permission boundary must be a page boundary, or one page would
       carry two permissions and the weaker one would win. */
    ASSERT((__kernel_start & 0xFFF) == 0, "kernel image is not page aligned")
    ASSERT((__text_start & 0xFFF) == 0, "kernel text does not start on a page")
    ASSERT((__text_end & 0xFFF) == 0, "kernel text does not end on a page")
    ASSERT((__rodata_start & 0xFFF) == 0, "kernel rodata starts mid-page")
    ASSERT((__rodata_end & 0xFFF) == 0, "kernel rodata ends mid-page")
    ASSERT((__data_start & 0xFFF) == 0, "kernel data does not start on a page")
    ASSERT((__bss_start & 0xFFF) == 0, "kernel bss does not start on a page")
    ASSERT((__kernel_end & 0xFFF) == 0, "kernel image does not end on a page")

    /* The permission map must cover the image with no gap between sections. */
    ASSERT(__text_end == __rodata_start, "a gap separates text and rodata")
    ASSERT(__rodata_end == __data_start, "a gap separates rodata and data")
    ASSERT(__data_end == __bss_start, "a gap separates kernel data and bss")
    ASSERT(__bss_end == __kernel_end, "kernel bss does not end the image")

    /*
     * paging.c gives the 2 MiB regions holding this image 4 KiB granularity and
     * keeps one fixed-size region descriptor for each, so the image has to stay
     * inside the count PAGING_MAX_KERNEL_FINE_REGIONS statically asserts.
     */
    ASSERT(__kernel_end <= 0x800000,
           "kernel image no longer fits the fine-grained paging window")
}

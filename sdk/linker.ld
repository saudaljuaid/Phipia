/* SPDX-License-Identifier: GPL-3.0-only */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x0000400000000000;
    .text : ALIGN(0x1000) {
        KEEP(*(.text.start))
        *(.text .text.* .ltext .ltext.*)
    } :text
    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.* .lrodata .lrodata.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :rodata
    . = ALIGN(0x1000);
    .data : {
        *(.data .data.* .ldata .ldata.*)
        *(.got .got.*)
    } :data
    .tdata : ALIGN(16) {
        __tls_start = .;
        *(.tdata .tdata.* .ltdata .ltdata.*)
        /* Keep PT_TLS well-formed for languages with no static TLS symbols. */
        BYTE(0)
        __tls_file_end = .;
    } :data :tls
    .tbss (NOLOAD) : {
        *(.tbss .tbss.* .ltbss .ltbss.*)
        __tls_end = .;
    } :data :tls
    .bss (NOLOAD) : {
        *(.bss .bss.* .lbss .lbss.* COMMON)
    } :data
    /DISCARD/ : {
        *(.comment .note .note.* .eh_frame .eh_frame_hdr .gcc_except_table)
        *(.llvm_addrsig .rela .rela.*)
    }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_addr 0 : { *(.debug_addr) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_ranges 0 : { *(.debug_ranges) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_str 0 : { *(.debug_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .symtab 0 : { *(.symtab) }
    .shstrtab 0 : { *(.shstrtab) }
    .strtab 0 : { *(.strtab) }
}

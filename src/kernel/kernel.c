/* SPDX-License-Identifier: GPL-3.0-only */
/* Zenith OS: the first freestanding kernel code. */
#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_BOOT_MAGIC UINT32_C(0x36D76289)
#define VGA_WIDTH 80U
#define VGA_HEIGHT 25U
#define VGA_MEMORY ((volatile uint16_t *)(uintptr_t)0xB8000U)
#define VGA_COLOR UINT8_C(0x0B)
#define COM1 UINT16_C(0x03F8)

static size_t console_row;
static size_t console_column;

__attribute__((noreturn)) void kernel_main(uint32_t magic, uintptr_t boot_info);

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void serial_initialize(void)
{
    outb(COM1 + 1U, 0U);     /* Disable interrupts. */
    outb(COM1 + 3U, 0x80U);  /* Enable divisor latch. */
    outb(COM1, 3U);          /* 38400 baud. */
    outb(COM1 + 1U, 0U);
    outb(COM1 + 3U, 0x03U);  /* Eight bits, no parity, one stop bit. */
    outb(COM1 + 2U, 0xC7U);  /* Enable and clear the FIFO. */
    outb(COM1 + 4U, 0x0BU);  /* Mark data terminal ready. */
}

static void serial_putc(char character)
{
    /* A bounded wait prevents a missing UART from freezing the kernel. */
    for (uint32_t attempt = 0; attempt < 100000U; ++attempt) {
        if ((inb(COM1 + 5U) & 0x20U) != 0U) {
            outb(COM1, (uint8_t)character);
            return;
        }
    }
}

static uint16_t vga_entry(char character)
{
    return (uint16_t)(uint8_t)character | ((uint16_t)VGA_COLOR << 8U);
}

static void console_clear(void)
{
    for (size_t index = 0; index < VGA_WIDTH * VGA_HEIGHT; ++index) {
        VGA_MEMORY[index] = vga_entry(' ');
    }
    console_row = 0;
    console_column = 0;
}

static void console_scroll(void)
{
    for (size_t row = 1; row < VGA_HEIGHT; ++row) {
        for (size_t column = 0; column < VGA_WIDTH; ++column) {
            VGA_MEMORY[(row - 1U) * VGA_WIDTH + column] =
                VGA_MEMORY[row * VGA_WIDTH + column];
        }
    }
    for (size_t column = 0; column < VGA_WIDTH; ++column) {
        VGA_MEMORY[(VGA_HEIGHT - 1U) * VGA_WIDTH + column] = vga_entry(' ');
    }
    console_row = VGA_HEIGHT - 1U;
}

static void console_putc(char character)
{
    serial_putc(character);
    if (character == '\n') {
        console_column = 0;
        ++console_row;
    } else {
        VGA_MEMORY[console_row * VGA_WIDTH + console_column] = vga_entry(character);
        if (++console_column == VGA_WIDTH) {
            console_column = 0;
            ++console_row;
        }
    }
    if (console_row == VGA_HEIGHT) {
        console_scroll();
    }
}

static void console_write(const char *text)
{
    while (*text != '\0') {
        console_putc(*text++);
    }
}

static void console_write_hex(uintptr_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    console_write("0x");
    for (unsigned shift = 60U;; shift -= 4U) {
        console_putc(digits[(value >> shift) & 0xFU]);
        if (shift == 0U) {
            break;
        }
    }
}

static __attribute__((noreturn)) void halt(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt" : : : "memory");
    }
}

void kernel_main(uint32_t magic, uintptr_t boot_info)
{
    serial_initialize();
    console_clear();
    if (magic != MULTIBOOT2_BOOT_MAGIC) {
        console_write("Zenith OS: invalid Multiboot2 handoff\n");
        halt();
    }
    console_write("Zenith OS: kernel online\n");
    console_write("Zenith OS: Multiboot2 info at ");
    console_write_hex(boot_info);
    console_putc('\n');
    console_write("Zenith OS: day one passed\n");
    halt();
}

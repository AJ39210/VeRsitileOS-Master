#include <stdint.h>

#include "panic.h"
#include "serial.h"
#include "vesa.h"

static void panic_write(const char *text)
{
    if (!text)
        return;
    while (*text)
        vesa_console_putc(*text++);
}

static void panic_write_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int32_t shift = 60; shift >= 0; shift -= 4)
        vesa_console_putc(digits[(value >> shift) & 0xF]);
}

static void halt_cpu(void) __attribute__((noreturn));

static void halt_cpu(void)
{
    __asm__ volatile ("cli");
    for (;;)
        __asm__ volatile ("hlt");
}

void kernel_panic(const char *message)
{
    __asm__ volatile ("cli");
    serial_write("\n*** KERNEL PANIC ***\n");
    serial_write(message ? message : "unspecified fatal error");
    serial_putc('\n');

    if (vesa_get_width() && vesa_get_height())
    {
        vesa_fill_rect(
            0, 0,
            vesa_get_width(),
            vesa_get_height(),
            0x170A0A
        );
        vesa_console_reset();
        vesa_console_putc('\n');
        panic_write("*** KERNEL PANIC ***\n\n");
        panic_write(message ? message : "unspecified fatal error");
        panic_write("\n\nSystem halted.\n");
    }

    halt_cpu();
}

void kernel_panic_exception(
    uint64_t vector,
    uint64_t error_code,
    uint64_t instruction_pointer
)
{
    static const char *names[] = {
        "Divide error", "Debug", "NMI", "Breakpoint",
        "Overflow", "BOUND range", "Invalid opcode",
        "Device unavailable", "Double fault", "Coprocessor overrun",
        "Invalid TSS", "Segment not present", "Stack-segment fault",
        "General protection fault", "Page fault", "Reserved",
        "x87 floating-point", "Alignment check", "Machine check",
        "SIMD floating-point", "Virtualization", "Control protection",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Hypervisor injection",
        "VMM communication", "Security exception", "Reserved"
    };

    __asm__ volatile ("cli");
    const char *name = vector < sizeof(names) / sizeof(names[0])
        ? names[vector]
        : "Unknown exception";

    serial_write("\n*** KERNEL PANIC: ");
    serial_write(name);
    serial_write(" (vector ");
    serial_write_dec(vector);
    serial_write(") ***\nError code: ");
    serial_write_hex(error_code);
    serial_write("\nInstruction pointer: ");
    serial_write_hex(instruction_pointer);
    serial_putc('\n');

    if (vesa_get_width() && vesa_get_height())
    {
        vesa_fill_rect(
            0, 0,
            vesa_get_width(),
            vesa_get_height(),
            0x170A0A
        );
        vesa_console_reset();
        vesa_console_putc('\n');
        panic_write("*** KERNEL PANIC ***\n\n");
        panic_write(name);
        panic_write("\nVector: 0x");
        panic_write_hex(vector);
        panic_write("\nError: 0x");
        panic_write_hex(error_code);
        panic_write("\nRIP: 0x");
        panic_write_hex(instruction_pointer);
        panic_write("\n\nSystem halted.\n");
    }
    halt_cpu();
}

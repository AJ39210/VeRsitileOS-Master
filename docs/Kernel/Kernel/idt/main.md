# Interrupt descriptor table

- [`idt.c`](../../../../Kernel/Kernel/idt/idt.c) initializes IDT gates and supports kernel, user-callable, and IST-configured entries.
- [`idt.asm`](../../../../Kernel/Kernel/idt/idt.asm) supplies low-level IDT loading.

The IDT connects processor exceptions and the configured syscall vector to their entry routines. It is initialized before the ISR layer.

API: [`Include/idt.h`](../../../../Kernel/Include/idt.h).

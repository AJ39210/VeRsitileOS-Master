# Kernel core

The `Kernel/Kernel/` directory contains boot coordination and core services. The initialization sequence is in [`kmain.c`](../../../Kernel/Kernel/kmain.c); details for nested areas are linked below.

## Files at this level

- [`kmain.c`](../../../Kernel/Kernel/kmain.c) — Limine requests, ordered initialization, hardware probing, framebuffer/font setup, and transition into later kernel work.
- [`block.c`](../../../Kernel/Kernel/block.c) — bounded block-device registry with validated sector reads/writes.
- [`panic.c`](../../../Kernel/Kernel/panic.c) — fatal error and exception reporting, using serial and framebuffer output when available.
- [`test.c`](../../../Kernel/Kernel/test.c) — kernel self-tests covering memory, devices, input, and related interfaces; `tests_run()` is the test entry point.
- [`certificate.md`](../../../Kernel/Kernel/certificate.md) — project certificate/documentation file; not part of kernel compilation.

## Subsystems

- [ACPI](acpi/main.md) — firmware table parsing used by APIC setup.
- [APIC](apic/main.md) — local I/O APIC and interrupt-controller setup.
- [GDT](gdt/main.md), [IDT](idt/main.md), [ISR](isr/main.md), and [IRQ](irq/main.md) — CPU tables and exception/hardware interrupt dispatch.
- [Memory management](mm/main.md) — PMM, paging, virtual memory, regions, heap, and spinlocks.
- [Processes and ELF](process/main.md) — tasking, user address spaces, descriptors, and ELF loading.
- [System calls](syscalls/main.md) — user-to-kernel ABI dispatch.
- [Timer](timer/main.md) — PIT tick/timekeeping.

Several assembly files provide entry stubs or low-level routines for their corresponding C modules. C/ASM source lists are defined in [`Kernel/config.cfg`](../../../Kernel/config.cfg).

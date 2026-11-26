# Global descriptor table

- [`gdt.c`](../../../../Kernel/Kernel/gdt/gdt.c) creates/loads the kernel GDT and updates kernel/IST stack pointers.
- [`gdt.asm`](../../../../Kernel/Kernel/gdt/gdt.asm) contains the architecture-specific load/transition code.

The GDT is initialized early in `kmain()`, before interrupt setup and memory-dependent services. Stack updates are used by tasking and interrupt handling.

API: [`Include/gdt.h`](../../../../Kernel/Include/gdt.h).

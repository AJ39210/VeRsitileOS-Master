# APIC

- [`apic.c`](../../../../Kernel/Kernel/apic/apic.c) initializes APIC hardware using ACPI-discovered addresses, handles end-of-interrupt signaling, and enables/disables supported IRQ lines.
- [`apic.asm`](../../../../Kernel/Kernel/apic/apic.asm) provides the low-level spurious-interrupt entry stub.

The boot sequence initializes APIC and then disables the legacy PIC before setting up the IRQ routing layer. Actual support depends on valid firmware tables and available hardware.

API: [`Include/apic.h`](../../../../Kernel/Include/apic.h).

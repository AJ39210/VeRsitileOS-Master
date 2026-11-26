# ACPI

[`acpi.c`](../../../../Kernel/Kernel/acpi/acpi.c) starts ACPI discovery from the firmware-provided RSDP physical address and HHDM offset. It provides firmware-table information needed by the APIC initialization path.

`kmain()` obtains the RSDP and HHDM values from Limine and passes them into `acpi_init()`. ACPI initialization is a prerequisite for the APIC setup used by the interrupt subsystem.

API: [`Include/acpi.h`](../../../../Kernel/Include/acpi.h).

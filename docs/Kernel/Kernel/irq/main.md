# Hardware IRQ layer

- [`irq.c`](../../../../Kernel/Kernel/irq/irq.c) installs IRQ stubs, maintains handler slots/counters, and dispatches registered handlers.
- [`irq.asm`](../../../../Kernel/Kernel/irq/irq.asm) saves/restores CPU state around hardware IRQ entry and calls the C dispatcher.
- [`pic.c`](../../../../Kernel/Kernel/irq/pic.c) provides legacy PIC remapping, mask/unmask, EOI, and disable operations.

Boot initializes the APIC, disables legacy PIC delivery, and initializes the IRQ layer. The PIC code remains available for legacy routing/control.

API: [`Include/irq.h`](../../../../Kernel/Include/irq.h) and [`Include/pic.h`](../../../../Kernel/Include/pic.h).

# Processor exceptions

- [`isr.c`](../../../../Kernel/Kernel/isr/isr.c) installs exception gates and dispatches exception frames to the kernel exception/panic path.
- [`isr.asm`](../../../../Kernel/Kernel/isr/isr.asm) generates the processor exception stubs and common register-saving entry.

The generated assembly covers the 32 architectural exception vectors and normalizes stack data before calling C. Unhandled/fatal exceptions are reported through the panic/logging facilities.

API and frame layout: [`Include/isr.h`](../../../../Kernel/Include/isr.h).

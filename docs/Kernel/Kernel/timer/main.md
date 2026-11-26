# PIT timer

[`pit.c`](../../../../Kernel/Kernel/timer/pit.c) configures the programmable interval timer and maintains tick/millisecond timekeeping. Other kernel services can query the current tick count or elapsed milliseconds.

`kmain()` treats timer initialization failure as fatal. The timer's IRQ is connected through the IRQ subsystem.

API: [`Include/timer.h`](../../../../Kernel/Include/timer.h).

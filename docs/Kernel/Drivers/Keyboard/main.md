# Keyboard input

- [`keyboard.c`](../../../../Kernel/Drivers/Keyboard/keyboard.c) maintains keyboard state and queues key events. It handles scancodes, reports input availability, reads event records, and tracks modifier state. Holding Ctrl and Print Screen (SysRq), then pressing K, deliberately invokes the kernel panic handler for testing.
- [`ps2.c`](../../../../Kernel/Drivers/Keyboard/ps2.c) initializes the legacy PS/2 keyboard path and feeds scancodes into the keyboard core. The core also polls the controller while user input is being read, so input does not depend exclusively on IRQ delivery.

The syscall read path can consume keyboard input as characters, while the event API is available for structured input consumers. PS/2, USB HID boot-keyboard, and VirtIO keyboard input enter the same scancode handler. USB support is for boot-keyboard reports, not every USB keyboard report format.

Interfaces: [`Include/keyboard.h`](../../../../Kernel/Include/keyboard.h) and [`Include/ps2.h`](../../../../Kernel/Include/ps2.h).

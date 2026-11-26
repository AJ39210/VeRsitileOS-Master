# Mouse input

[`mouse.c`](../../../../Kernel/Drivers/Mouse/mouse.c) owns mouse initialization status and input event handling. It accepts USB report deltas/buttons, and exposes event reads for consumers. `kmain()` attempts PS/2 mouse initialization during boot; USB reports arrive through the HID path.

This source is the shared input-side handler, not a USB host controller driver. The controller and report parsing layers are documented in [USB xHCI](../Usb/main.md) and [HID](../HID/main.md).

API: [`Include/mouse.h`](../../../../Kernel/Include/mouse.h).

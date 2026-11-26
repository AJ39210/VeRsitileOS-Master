# HID report handling

[`hid.c`](../../../../Kernel/Drivers/HID/hid.c) handles the USB HID boot keyboard and boot mouse report formats used by the current input path. It initializes its state and translates keyboard/mouse report data into the kernel's input handling interfaces.

This module parses reports; controller discovery, USB transfers, and polling belong to the USB/xHCI path. A working HID parser alone does not imply support for arbitrary HID report descriptors or every USB device.

The public API is in [`Include/hid.h`](../../../../Kernel/Include/hid.h). Keyboard and mouse consumers are documented under [Keyboard](../Keyboard/main.md) and [Mouse](../Mouse/main.md).

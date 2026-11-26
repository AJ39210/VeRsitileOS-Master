# USB xHCI

[`xhci.c`](../../../../Kernel/Drivers/Usb/xhci.c) contains xHCI controller matching/probing, controller state, command/event handling, and polling support. Boot probes xHCI after PCI enumeration. Its controller count and device match helpers are also used by kernel tests and device reporting.

USB class behavior is split into other modules: [`Drivers/HID/hid.c`](../HID/main.md) parses boot keyboard/mouse reports, while the keyboard and mouse modules hold the OS-facing input state. Do not treat xHCI discovery as general USB Mass Storage support unless the corresponding mass-storage path is present and wired.

API: [`Include/xhci.h`](../../../../Kernel/Include/xhci.h).

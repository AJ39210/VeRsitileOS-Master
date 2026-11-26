# Device drivers

The driver sources live under [`Kernel/Drivers/`](../../../Kernel/Drivers/). Most are compiled according to the corresponding `DRIVER_ENABLE_<path>` setting in [`Kernel/config.cfg`](../../../Kernel/config.cfg); all listed drivers start enabled.

## Driver groups

- [Ethernet](Ethernet/main.md) — e1000 packet I/O and shared network protocols.
- [GOP](GOP/main.md) — UEFI framebuffer initialization through the VESA framebuffer backend.
- [HID](HID/main.md) — USB boot-protocol keyboard and mouse report parsing.
- [Keyboard](Keyboard/main.md) — key event state and legacy PS/2 keyboard input.
- [Mouse](Mouse/main.md) — mouse event handling and PS/2 initialization.
- [PCI](PCI/main.md) — PCI enumeration, configuration access, and device metadata.
- [USB xHCI](Usb/main.md) — xHCI controller/device discovery and polling.
- [VirtIO](Virtio/main.md) — PCI transport, queues, and block/input/GPU/network device support.
- [Wi-Fi](WiFi/main.md) — AR5007EG/AR2425 PCI/MMIO discovery groundwork and Intel 8265/8275 detection/firmware validation; neither adapter can yet scan or connect.
- [Serial](serial/main.md) — COM-port output, boot logs, and serial console support.
- [VESA framebuffer](vesa/main.md) — software framebuffer drawing, text console, and PSF2 font use.

## Initialization and dependencies

The boot sequence in [`Kernel/Kernel/kmain.c`](../../../Kernel/Kernel/kmain.c) initializes PCI before PCI-dependent probes, then HID and xHCI, and initializes VirtIO block/input devices when PCI is available. After systemd starts, it loads the optional GPU and network modules from the versioned module directory, then initializes the shared network stack. VirtIO-net is preferred, with e1000 as fallback.

Driver toggles remove source files from the build; they do not conditionally remove calls to those drivers from the rest of the kernel. For example, disabling a driver that another file references can lead to unresolved symbols. Keep dependencies enabled together or update the calling code and relevant configurations.

## Understand API versus hardware status

The interfaces are declared in [`Kernel/Include/`](../../../Kernel/Include/). Some drivers implement usable operations, while others are currently detection/probe foundations. The individual pages call out known limitations observed in the source.

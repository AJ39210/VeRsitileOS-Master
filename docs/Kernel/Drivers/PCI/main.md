# PCI enumeration

[`pci.c`](../../../../Kernel/Drivers/PCI/pci.c) performs PCI configuration-space access and device enumeration. It provides device counts/index lookup, BDF formatting, class/subclass lookup, memory-enable support, and formatting helpers used by device-facing pseudo-filesystems.

PCI enumeration is an early dependency for xHCI, VirtIO, Wi-Fi, Ethernet, and GPU probing. `kmain()` calls `pci_init()` before those probes and continues boot with a warning if enumeration fails.

The public device representation and API are in [`Include/pci.h`](../../../../Kernel/Include/pci.h). Keep PCI enabled when building any source that references these APIs.

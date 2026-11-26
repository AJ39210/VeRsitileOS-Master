# VirtIO devices

- [`virtio_pci.c`](../../../../Kernel/Drivers/Virtio/virtio_pci.c) discovers and enables VirtIO PCI devices and provides modern transport/queue setup and notification helpers.
- [`virtio_legacy.c`](../../../../Kernel/Drivers/Virtio/virtio_legacy.c) contains legacy transport queue/memory helpers.
- [`virtio_blk.c`](../../../../Kernel/Drivers/Virtio/virtio_blk.c) initializes discovered VirtIO block devices and reports device count.
- [`virtio_input.c`](../../../../Kernel/Drivers/Virtio/virtio_input.c) initializes and polls VirtIO input devices.
- [`virtio_gpu.c`](../../../../Kernel/Drivers/Virtio/virtio_gpu.c) initializes the VirtIO GPU path and presents updates through its current API.
- [`virtio_net.c`](../../../../Kernel/Drivers/Virtio/virtio_net.c) is built as the optional `virtio_net.ko` module and supplies packet send/receive and link state for the shared network layer. Systemd loads it after PCI enumeration and before network initialization.

VirtIO depends on PCI enumeration. The boot path initializes PCI transport discovery before the built-in block/input drivers; GPU setup occurs after framebuffer initialization. VirtIO network is independently initialized by its module after systemd starts. See [`Ethernet/network.c`](../Ethernet/main.md) for backend selection.

Headers for each interface are in [`Include/`](../../../../Kernel/Include/): `virtio_pci.h`, `virtio_legacy.h`, `virtio_blk.h`, `virtio_input.h`, `virtio_gpu.h`, and `virtio_net.h`.

The exact amount of device functionality is defined by each implementation. A configured build flag controls whether a source is compiled; it does not automatically remove dependent calls.

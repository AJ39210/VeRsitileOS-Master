# Ethernet and network stack

Files:

- [`e1000.c`](../../../../Kernel/Drivers/Ethernet/e1000/e1000.c) provides initialization, link status, packet send, and packet receive for supported Intel e1000-class PCI hardware. It is built as `e1000.ko`.
- [`rtl810x.c`](../../../../Kernel/Drivers/Ethernet/Realtek/rtl810x.c) implements the shared descriptor-ring transport, per-MAC-revision initialization, and PHY setup for PCIe RTL8102E/RTL8103E adapters. Revision signatures are identified in [`rtl8102e.c`](../../../../Kernel/Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.c) and [`rtl8103e.c`](../../../../Kernel/Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.c); the family is packaged as `realtek_rtl810x.ko`.
- [`network.c`](../../../../Kernel/Drivers/Ethernet/network.c) selects a packet backend and implements shared network-facing operations.
- [`virtio_net.c`](../../../../Kernel/Drivers/Virtio/virtio_net.c) is built as `virtio_net.ko` and provides the VirtIO packet backend; its details are on the [VirtIO page](../Virtio/main.md).

Systemd loads these optional device modules, then starts the built-in shared network layer. It tries initialized VirtIO networking first, then RTL810x, then e1000. The RTL8102E and RTL8103E use the same RTL8168-family descriptor format and PCI device ID, so they share a transport module rather than competing modules binding the same adapter. A small operation-table ABI separates packet transport from shared protocol code. The network layer exposes initialization/polling, interface information, ICMP ping, and hostname resolution APIs. `network_poll()` is the service hook used to process incoming traffic and protocol progress.

The public interfaces are in [`Include/network.h`](../../../../Kernel/Include/network.h) and [`Include/e1000.h`](../../../../Kernel/Include/e1000.h). The syscall layer exposes selected network operations to user programs.

Network support depends on PCI enumeration, a working packet backend, and the network layer's protocol implementation. A detected PCI adapter alone does not establish a configured network connection. RTL8102E/RTL8103E hardware operation has not yet been validated on physical hardware.

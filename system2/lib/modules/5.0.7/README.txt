VeRsitileOS kernel modules for version 5.0.7 are installed in this directory.
The AMD Radeon, Intel HD Graphics 2000, and NVIDIA GeForce 8400 GS modules
include read-only firmware scanout probes and may attach the software DRM
framebuffer path; they do not provide native modesetting or acceleration.
Optional e1000 Ethernet, VirtIO network, and VirtIO GPU drivers are also
built from kernel sources and loaded by systemd. The early framebuffer,
input, storage, filesystem, PCI, USB, and networking foundations remain
built into the kernel because boot and runtime code still depends on their
early initialization or direct interfaces.

# GPU support

The separate [`gpu/`](../../../Kernel/gpu/) tree contains the AMD Radeon HD, Intel HD Graphics 2000, and NVIDIA GeForce 8400 GS probe implementations. They are built as separate versioned kernel modules and loaded by systemd after PCI enumeration. These probes should be distinguished from:

- the software framebuffer renderer in [`Drivers/vesa/`](../Drivers/vesa/main.md),
- the UEFI framebuffer adapter in [`Drivers/GOP/`](../Drivers/GOP/main.md),
- the software-framebuffer DRM API in [`drm/`](../drm/main.md), and
- the VirtIO GPU path in [`Drivers/Virtio/`](../Drivers/Virtio/main.md).

The AMD Radeon HD, Intel HD Graphics 2000, and NVIDIA GeForce 8400 GS modules
include conservative read-only firmware scanout probes and may attach the
generic software framebuffer DRM when the hardware state is compatible. They
do not perform native modesetting or hardware acceleration. See
[AMD Radeon HD](amd/RadeonHD/main.md) for details on the vendor-specific path.

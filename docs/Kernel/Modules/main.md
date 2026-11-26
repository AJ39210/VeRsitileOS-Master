# Kernel modules

The kernel accepts versioned x86-64 ELF relocatable objects (`ET_REL`) from
`/lib/modules/<kernel-version>/`. PID 1 requests a load through
`OS_SYS_MODULE_LOAD`; the kernel rejects requests from other tasks and paths
outside the matching versioned module directory.

The build produces separate `radeonhd.ko`, `intel_hd2000.ko`,
`nvidia_8400gs.ko`, `e1000.ko`, `virtio_net.ko`, and `virtio_gpu.ko` modules.
Systemd loads them after PCI enumeration. The kernel retains a small Radeon
metadata dispatch interface so existing sysfs and devfs paths continue to
work after the probe implementation moves into its module. Ethernet and
VirtIO network modules register device-operation tables with the built-in
networking stack; systemd starts networking only after module loading, so
DHCP and packet processing use a registered backend. The VirtIO GPU module
similarly registers presentation operations; the boot framebuffer remains
available when no VirtIO GPU device or module is present.

The Radeon HD module validates and maps its MMIO BAR, reads (but does not
write) the Evergreen/Northern Islands CRTC0 control and timing registers, and
can attach the existing software DRM framebuffer path when the active
firmware scanout mode matches the boot framebuffer. This provides basic
framebuffer allocation, damage tracking, and presentation without resetting
the GPU or changing display modes. It does not provide hardware acceleration,
connector detection, or native modesetting; if the CRTC is inactive or its
mode differs from the boot framebuffer, the firmware display remains
untouched and the generic framebuffer path stays active.

GPU-specific DRM code is kept separate from the generic framebuffer API:
AMD is under `Kernel/drm/amd/`, while Intel and NVIDIA have separate
`Kernel/drm/intel/` and `Kernel/drm/nvidia/` interfaces. The Intel and NVIDIA
implementations perform read-only firmware scanout checks and can attach the
software framebuffer path when it is safe to do so. Intel HD Graphics 2000
reads the Sandy Bridge pipe enable and source-size registers and requires an
exact mode match. GeForce 8400 GS checks the NV4x graphics scanout selection
bit and uses the boot-provided framebuffer dimensions; it does not independently
decode timing registers. Neither implementation changes modes or enables
hardware acceleration. The vendor-neutral software framebuffer API remains
in `Kernel/Include/drm.h` and its core implementation in
`Kernel/drm/code/drm.c`.

Modules must export `vos_module_abi_version` (currently `1`) and
`vos_module_init(void)`. The loader also runs validated ELF `.init_array`
constructors after relocation and before the module entrypoint. It accepts
only the relocation types needed by the current toolchain output, resolves
imports against a small explicit export table, and enforces section
permissions after relocation. The current module interface has no unload
operation or dependency resolver.

The early display/input stack, PCI, USB/HID, VirtIO storage/input, Wi-Fi,
storage, and filesystem implementations remain built in. They initialize
before systemd or are consumed through direct kernel calls.
In particular, filesystem drivers are directly coupled to VFS and are needed
before systemd starts. Moving those drivers requires additional registration
and boot-order work rather than just compiling them as relocatable objects.
Networking protocol code also stays built in, but its e1000 and VirtIO packet
transports now register through separate module operations tables; systemd
starts that protocol layer once module discovery finishes.

The build installs these modules in `system/lib/modules/5.0.7/` in the source
tree, which is packaged at `/lib/modules/5.0.7/` in the boot image. From the
repository root, `make -C Kernel modules` builds and installs the modules, and
`make -C Kernel iso` packages them into the boot image.

# AMD Radeon HD driver

[`radeonhd.c`](../../../../../Kernel/gpu/amd/RadeonHD/radeonhd.c) detects
supported Evergreen and Northern Islands PCI devices, records probe
statistics, and formats the attributes consumed by sysfs and devfs. The
driver is packaged in `system/lib/modules/5.0.7/radeonhd.ko` and loaded by
systemd.

AMD-specific DRM code lives in
[`amd_drm.c`](../../../../../Kernel/drm/amd/amd_drm.c), with its interface in
[`amd_drm.h`](../../../../../Kernel/Include/amd_drm.h). It validates/maps the
GPU register BAR, reads CRTC0 status and timing without writing GPU
registers, and attaches software DRM only when the detected firmware mode
matches the boot framebuffer. Framebuffer drawing, damage tracking, and
presentation are available, but this is not native modesetting or
acceleration. If probing fails or the modes do not match, the boot-provided
framebuffer remains unchanged.

The generic [`drm.h`](../../../../../Kernel/Include/drm.h) remains the
vendor-neutral software framebuffer API used by kernel and syscall code.
Intel and NVIDIA have separate vendor-specific DRM paths under
`Kernel/drm/intel/` and `Kernel/drm/nvidia/`; they probe their respective
firmware scanout state without writing GPU registers. Intel reads the active
pipe source dimensions and attaches only on a boot-framebuffer mode match.
The GeForce 8400 GS checks the NV4x non-VGA scanout selection bit and keeps
the boot framebuffer mode; it does not independently decode NVIDIA timing
registers. Neither path performs native modesetting or acceleration.

Radeon probe API:
[`radeonhd.h`](../../../../../Kernel/Include/radeonhd.h).

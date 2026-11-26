# DRM-style framebuffer core

[`drm/code/drm.c`](../../../Kernel/drm/code/drm.c) provides a kernel drawing/framebuffer API on top of the initialized VESA framebuffer. The current device is named `vesa-drm0`; device information explicitly reports no modesetting or hardware acceleration and does report software framebuffer and partial-update support.

The implementation manages a bounded set of software framebuffers, validates/clips drawing operations, tracks damage regions, and presents whole or partial updates. Convenience routines draw rectangles/text and blit/read pixels. User drawing syscalls route through this API.

This is a small DRM-like interface, not a full Linux DRM/KMS subsystem. It does not provide a GPU driver abstraction, hardware modesetting, or accelerated command submission.

Public types and limits are in [`Include/drm.h`](../../../Kernel/Include/drm.h). Initialization is performed after a framebuffer backend is ready in [`Kernel/Kernel/kmain.c`](../../../Kernel/Kernel/kmain.c).

# DRM framebuffer implementation

[`drm.c`](../../../../Kernel/drm/code/drm.c) is the implementation of the project's small DRM-style software display API. It registers the existing VESA framebuffer as `vesa-drm0`, creates bounded heap-backed software framebuffers, validates/clips drawing bounds, records damaged areas, and presents full or partial regions to the VESA framebuffer.

The implementation supports XRGB8888 software buffers and convenience operations for rectangle fill, text drawing, pixel blit, and pixel readback. It reports no hardware acceleration and no modesetting; see the [DRM overview](../main.md).

Public API and data structures: [`Include/drm.h`](../../../../Kernel/Include/drm.h).

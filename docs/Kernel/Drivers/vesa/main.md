# Software framebuffer and text console

- [`vesa.c`](../../../../Kernel/Drivers/vesa/vesa.c) initializes the Limine framebuffer, exposes geometry/pitch, draws pixels/rectangles/text, supports RGB blit/read operations, and implements a text console.
- [`font.h`](../../../../Kernel/Drivers/vesa/font.h) provides the built-in bitmap font data used by the framebuffer text path/fallback.

The framebuffer backend can load PSF2 font data through `vesa_load_psf2()`. Boot attempts to load the system font from `/fonts/terminus.psf`; if unavailable, the built-in font remains available. Console history replay is limited to the visible tail, and framebuffer scrolling uses a bulk row copy to avoid redrawing each pixel in nested loops. This is not a general TTF font loader.

UEFI framebuffer selection is wrapped by [`Drivers/GOP/gop.c`](../GOP/main.md). The DRM compatibility core is separate and documented in [DRM](../../drm/main.md).

API: [`Include/vesa.h`](../../../../Kernel/Include/vesa.h).

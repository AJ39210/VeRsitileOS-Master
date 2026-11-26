# UEFI GOP framebuffer

[`gop.c`](../../../../Kernel/Drivers/GOP/gop.c) is a small UEFI-facing framebuffer initialization adapter. `gop_init()` accepts the framebuffer information supplied by Limine and delegates the actual framebuffer setup to the shared software renderer in [`Drivers/vesa/vesa.c`](../vesa/main.md). It also exposes whether GOP initialization succeeded.

The boot code selects this path for EFI64 firmware and uses the direct framebuffer initialization path otherwise. GOP here describes framebuffer acquisition; this file is not a GPU acceleration driver or a full mode-setting implementation.

API: [`Include/gop.h`](../../../../Kernel/Include/gop.h).

# Kernel source guide

This documentation mirrors the source layout in [`Kernel/`](../../Kernel/). Each page explains the files in its corresponding source folder and how that code fits into the current boot/build flow.

## Source areas

- [Block devices](Block/main.md) — ATA PIO, ATAPI, and the shared block-device interface.
- [Drivers](Drivers/main.md) — device drivers grouped by hardware family.
- [DRM](drm/main.md) — framebuffer, drawing, and presentation API.
- [Filesystems](fs/main.md) — VFS and mounted filesystem implementations.
- [Kernel core](Kernel/main.md) — boot, memory, interrupts, processes, syscalls, and timer.
- [GPU support](gpu/main.md) — AMD Radeon HD detection and probe reporting.
- [Kernel modules](Modules/main.md) — relocatable ELF loading, optional GPU probe modules, and migration limits.
- [Public headers](Include/main.md) — interfaces shared across kernel subsystems.
- [Build and configuration tools](tools/main.md) — Make-based build settings and configuration app.
- [User test program](User/main.md) — the kernel's ELF/user-mode integration test.

## Boot and subsystem relationships

[`Kernel/Kernel/kmain.c`](../../Kernel/Kernel/kmain.c) owns initialization. Broadly, it initializes serial logging, validates the Limine responses, installs CPU tables, initializes memory management, APIC/IRQ/timer, sets up input and syscalls, mounts the VFS, enumerates PCI devices, initializes boot-time device support, scans storage, starts networking, and initializes a framebuffer/DRM display when available. After the kernel tests and userspace startup, systemd loads the optional GPU probe modules from the versioned module directory.

Storage drivers register block devices through [`Kernel/Kernel/block.c`](../../Kernel/Kernel/block.c). The MBR/GPT scanner and FAT32 support use that generic interface. The preferred root filesystem is a read-only USTAR archive loaded by Limine; ISO9660 over legacy ATAPI remains a fallback. This avoids requiring the boot ISO to be visible to the kernel as an optical drive, as is common when booting through USB ISO loaders.

PCI and VirtIO are shared foundations for several drivers. Disabling an individual source in `config.cfg` can therefore break another compiled component that calls its functions; see the [driver guide](Drivers/main.md) and [build guide](tools/main.md).

## Build and configuration

From the repository root, `make -C Kernel` builds the ISO. `make -C Kernel kernel` builds only the kernel ELF, `make -C Kernel cfg` opens the host configuration app, and `make -C Kernel/tools/configuration_app check` validates the version manifest and build configuration.

[`Kernel/config.cfg`](../../Kernel/config.cfg) is included by [`Kernel/Makefile`](../../Kernel/Makefile). It contains compiler/linker settings, source lists, and `DRIVER_ENABLE_<source> = y|n` selections. All declared driver selections currently default to `y`. The graphics-library target uses a build stamp and tracks library inputs to avoid rebuilding unchanged libraries.

[`Kernel/Version.cfg`](../../Kernel/Version.cfg) is a separate per-file version manifest; it is not the build settings file. The configuration app can update both the file-version manifest and driver selections.

The Rust panic handler is built with `rustc` for `x86_64-unknown-none`; install that target with `rustup target add x86_64-unknown-none` if it is missing.

## Scope and implementation status

These pages describe the checked-in implementation, not promises of complete hardware compatibility. Some pieces are foundations or probes rather than full device implementations. In particular, Intel 8265 Wi-Fi support detects the PCI device, prepares bounded PCI/MMIO/DMA resources, reads and validates API-36 firmware, and stages init/runtime chunks in memory; it does not upload firmware, start the radio, scan for networks, or transport Wi-Fi packets. Radeon HD support currently reports probe/classification information rather than providing a complete accelerated display driver. Consult the corresponding driver pages before relying on a feature.

 thats it for new :3
 

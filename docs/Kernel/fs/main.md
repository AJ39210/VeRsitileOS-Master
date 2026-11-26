# Filesystem and VFS

[`fs/`](../../../Kernel/fs/) contains the filesystem implementations and partition scanner. [`Kernel/Kernel/block.c`](../../../Kernel/Kernel/block.c) provides the shared sector-device registry used by disk-backed filesystems.

## Components

- [VFS](vfs/main.md) — validates paths and routes operations across mounted filesystems.
- [devfs](devfs/main.md) — device-oriented files such as console/input/framebuffer and device metadata.
- [procfs](procfs/main.md) — process/kernel runtime information.
- [sysfs](sysfs/main.md) — system and discovered-device attributes.
- [FAT32](fat32/main.md) — formatting, mounting, file/directory read-write operations for supported FAT32 volumes.
- [Installer](installer.md) — interactive disk installation, GPT layout, system copy, and BIOS/UEFI boot setup.
- [ISO9660](ISO9660/main.md) — boot-media filesystem used for the root tree.
- [`mbr.c`](../../../Kernel/fs/mbr.c) — scans disk devices for MBR/GPT partition tables.

The VFS initializes `/dev`, `/proc`, and `/sys`, mounts a Limine-loaded read-only USTAR initramfs as `/` when available, and falls back to ISO9660 over legacy ATAPI. It can later mount discovered FAT32 partitions. On an installed boot, the kernel can select the labeled ext2 system partition as `/` when the bootloader supplies `root=VeRsitileOS`. The initramfs lets the kernel read its root files without depending on the boot media being exposed as an IDE optical drive. The supported operations and filesystem limitations are implementation-specific; see each page.

Public filesystem interfaces are declared under [`Include/`](../../../Kernel/Include/), including `vfs.h`, `devfs.h`, `procfs.h`, `sysfs.h`, `fat32.h`, `iso9660.h`, and `mbr.h`.

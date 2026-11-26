# Virtual filesystem

[`vfs.c`](../../../../Kernel/fs/vfs/vfs.c) validates absolute paths, maintains mount/runtime directory state, and dispatches open/read/write/list/directory/create/remove operations to the relevant filesystem implementation.

Current routing includes:

- `/` — a read-only USTAR archive loaded as a Limine module, with ISO9660 over ATAPI as fallback; an installed boot can replace this with the labeled ext2 root when `root=VeRsitileOS` is present in the kernel command line.
- `/dev` — device pseudo-filesystem.
- `/proc` — process/runtime information.
- `/sys` — system/device information.
- discovered FAT32 volumes — mounted through the VFS partition-mount helper.
- installer target volumes — mounted temporarily at `/mnt/installer-root` (ext2) and `/mnt/installer-efi` (FAT32) while installation copies files and writes boot configuration.
- a bounded in-memory directory set for runtime-created directories.

The VFS prevents malformed paths such as parent traversal and repeated separators. The initramfs module is packaged at `/boot/system-root.tar` in the ISO and is loaded by Limine before the kernel starts. This avoids requiring a booted ISO to appear as a legacy IDE/ATAPI CD-ROM, which improves compatibility with USB ISO loaders such as Ventoy. The VFS interface is used by syscalls and kernel consumers such as font loading.

API: [`Include/vfs.h`](../../../../Kernel/Include/vfs.h).

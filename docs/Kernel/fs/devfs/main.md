# devfs

[`devfs.c`](../../../../Kernel/fs/devfs/devfs.c) exposes kernel devices through filesystem-style paths. It provides directory checks, size queries, listing, reads, and writes, and accepts CD-ROM registration from the VFS. Device entries include the console and available keyboard, framebuffer, block, PCI, and related device views as implemented in the source.

This is an in-kernel filesystem interface; it does not access host `/dev` nodes. Calls are routed by the VFS and user-facing file syscalls.

API: [`Include/devfs.h`](../../../../Kernel/Include/devfs.h).

# sysfs

[`sysfs.c`](../../../../Kernel/fs/sysfs/sysfs.c) exposes system and hardware attributes through directory checks, file-size queries, listing, and reads. It formats PCI device/resource attributes and Radeon HD probe/debug information through the kernel's module dispatch interface; before the Radeon module loads, Radeon-specific attributes report an unbound fallback.

The VFS mounts `/sys` during initialization. The attributes are generated from live kernel state; they are not ordinary writable configuration files.

API: [`Include/sysfs.h`](../../../../Kernel/Include/sysfs.h).

# FAT32

[`fat32.c`](../../../../Kernel/fs/fat32/fat32.c) supports FAT32 formatting, mounting/unmounting, path-to-volume matching, directory checks/listing/creation, and file reading/writing including creation and truncation. Formatting creates a FAT32 volume with mirrored FATs and primary/backup FSInfo sectors.

The VFS searches registered FAT32 volumes when resolving paths and routes writable file and directory operations to writable mounted volumes. Boot scans disks/partitions first, then asks the VFS to mount supported FAT32 partitions. The code defines finite volume and path/resource limits; it is not a general-purpose FAT implementation with every FAT variant or operation.

API and volume/file structures: [`Include/fat32.h`](../../../../Kernel/Include/fat32.h).

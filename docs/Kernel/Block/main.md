# Block devices

Source: [`Kernel/Block/`](../../../Kernel/Block/) and the shared registry in [`Kernel/Kernel/block.c`](../../../Kernel/Kernel/block.c).

## Files and responsibilities

- [`ata_pio.c`](../../../Kernel/Block/ata_pio.c) probes legacy ATA channels using port I/O and registers supported disk devices with the block layer.
- [`ATAPI/atapi.c`](../../../Kernel/Block/ATAPI/atapi.c) implements packet-device setup and sector reads for ATAPI devices such as an optical drive. The VFS retains it as a fallback for reading an ISO root.
- [`Kernel/Kernel/block.c`](../../../Kernel/Kernel/block.c) keeps a bounded registry of block devices and validates reads/writes before calling a driver's callbacks.

## Data flow

Drivers publish a device description (name, sector count/size, writable state, context, and read/write callbacks). Filesystem and partition code use a device index plus LBA/count; drivers own the hardware-specific operations. The generic layer requires the current fixed sector size and rejects out-of-range requests.

[`fs/mbr.c`](../../../Kernel/fs/mbr.c) scans registered disks for partition tables. [`fs/fat32/fat32.c`](../../../Kernel/fs/fat32/fat32.c) mounts supported FAT32 partitions. The preferred root is a read-only USTAR initramfs loaded by Limine; the VFS falls back to ISO9660 over ATAPI if that module is unavailable.

## Notes

- The registry has a fixed capacity; registration fails when it is full or when required callbacks/geometry are invalid.
- Write support is conditional: a device must declare itself writable and provide a write callback.
- A device being detected does not mean every filesystem on it is supported.
- Public data structures and callback signatures are declared in [`Include/block.h`](../../../Kernel/Include/block.h), [`Include/ata_pio.h`](../../../Kernel/Include/ata_pio.h), and [`Include/atapi.h`](../../../Kernel/Include/atapi.h).

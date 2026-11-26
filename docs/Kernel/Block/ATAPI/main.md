# ATAPI optical-device access

[`atapi.c`](../../../../Kernel/Block/ATAPI/atapi.c) implements legacy ATA task-file port I/O for ATAPI packet devices. It identifies/initializes a selected device and issues READ(10) packet commands to read logical sectors into a caller buffer.

The VFS probes common primary/secondary and master/slave legacy IDE positions, initializes an ATAPI candidate, and supplies its sector reader to ISO9660 as a fallback root filesystem. The preferred root is a read-only USTAR archive loaded by Limine, so boot does not depend on the USB device being exposed as an IDE/ATAPI CD-ROM.

This is not a USB mass-storage implementation and does not provide generic ATAPI write support.

Device structure and API: [`Include/atapi.h`](../../../../Kernel/Include/atapi.h).

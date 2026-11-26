# ISO9660

[`iso9660.c`](../../../../Kernel/fs/ISO9660/iso9660.c) provides initialization over a caller-supplied sector-read callback, root-directory access, directory listing, path lookup, and file reads. The VFS can mount it from an ATAPI boot CD-ROM as a fallback when the Limine-loaded USTAR root archive is unavailable.

The reader is sector/callback based: ISO9660 does not own the hardware transport. ATAPI reads are provided by [ATAPI](../../Block/main.md).

API: [`Include/iso9660.h`](../../../../Kernel/Include/iso9660.h).

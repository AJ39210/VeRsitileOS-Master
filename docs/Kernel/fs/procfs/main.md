# procfs

[`procfs.c`](../../../../Kernel/fs/procfs/procfs.c) provides a virtual `/proc` tree with directory detection, file-size queries, and reads generated from current kernel/process state. It is not backed by disk sectors.

The VFS mounts `/proc` during initialization. Its contents reflect the attributes implemented in this source and should be treated as runtime diagnostic interfaces, not persistent files.

API: [`Include/procfs.h`](../../../../Kernel/Include/procfs.h).

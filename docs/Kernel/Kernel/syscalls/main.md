# System call interface

- [`syscall.c`](../../../../Kernel/Kernel/syscalls/syscall.c) installs the user-callable syscall gate and dispatches the ABI. It validates user memory, bounds I/O/path/screen requests, and routes operations to tasking, VFS, input, networking, DRM, and socket helpers.
- [`syscall.asm`](../../../../Kernel/Kernel/syscalls/syscall.asm) is the low-level syscall entry stub that saves state and enters the C dispatcher.

Implemented service areas include descriptor read/write/open and file operations, directory listing, keyboard/mouse input, process/task operations, drawing/screen-region access, networking calls, and local socket pairs/readiness. The exact numeric IDs, argument structures, and error conventions are defined by [`Include/syscall.h`](../../../../Kernel/Include/syscall.h), [`Include/user_syscall.h`](../../../../Kernel/Include/user_syscall.h), and the shared user headers under `libc/include`.

Syscall handlers must validate all user pointers and lengths before copying or accessing memory. Follow the established `task_user_range_valid`, `task_copy_from_user`, and `task_copy_to_user` pattern.

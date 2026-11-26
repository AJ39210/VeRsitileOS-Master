# Tasks, user processes, and ELF loading

- [`task.c`](../../../../Kernel/Kernel/process/task.c) manages task slots, current/active task information, kernel/user stacks and address spaces, user-memory validation/copying, allocation/mapping/protection, process creation, file descriptors, child output, and local socket pairs/readiness.
- [`elf_loader.c`](../../../../Kernel/Kernel/process/elf_loader.c) validates and loads supported x86-64 ELF executables, maps loadable segments, and applies the relocations/symbol handling implemented by the loader.
- [`user_payload.asm`](../../../../Kernel/Kernel/process/user_payload.asm) contains the assembly payload/entry support used by the task startup path.

The loader supports a constrained executable format and reports unsupported dependencies; it is not a general dynamic-linker implementation. Tasking and loader limits are encoded in the implementation.

If the initial `/bin/terminal` ELF cannot be loaded or spawned, startup now reports the failure and exits the bootstrap user task instead of invoking the intentional kernel-panic syscall. Check the serial log for the `ELF loader:` reason and the `ELF spawn failed:` path/error. This keeps the kernel alive for diagnosis but does not provide a terminal fallback.

APIs and task/process structures: [`Include/task.h`](../../../../Kernel/Include/task.h) and [`Include/elf_loader.h`](../../../../Kernel/Include/elf_loader.h). The ELF integration test is documented in [User](../../User/main.md).

# Kernel user-mode test program

[`elf_test.c`](../../../Kernel/User/elf_test.c) is a user-mode ELF integration test built by the kernel Makefile. It validates initialized data/BSS, syscall argument checks, anonymous `mmap`/`mprotect`/`munmap`, local Unix stream socket pairs and `poll`, libc allocation/string/formatting/sort/setjmp/file I/O behavior, and runtime initialization/use of FreeType, libpng, and zlib.

The build target is `build/user/elf_test`; the source and user compile/link flags are declared in [`Kernel/config.cfg`](../../../Kernel/config.cfg). It links against the project CRT and libc plus the graphics libraries configured for this test.

This is a test program, not the system shell or desktop environment. The system terminal and command binaries are built from the separate `system/src/` tree. The system terminal is console-only; Neofetch reads its `Desktop Environment:` value from `/etc/system/desktop`. The test program assumes the runtime files and libraries expected by its checks are present in the system image.

# VCC

VCC is a small native VeRsitileOS compiler written in C. Its backend emits
x86-64 NASM assembly, and the OS build links the compiler with the existing
assembly startup code and libc into `system/bin/vcc`.

Run `vcc source.c` to create `/tmp/a.out`, or use `vcc source.c -o program`
to select an output path. The compiler emits a static x86-64 ELF directly, so
no guest assembler or linker command is needed. Run a program with
`./a.out`, `/tmp/a.out`, or its path relative to the current directory. Use
`vcc -S source.c` to print NASM-style assembly instead.

Generated files live in the writable RAM filesystem overlay. They are
available to the VFS, the ELF loader, and `ls` for the current boot, but are
lost on reboot. The overlay supports up to 64 files and 32 MiB of allocated
file data.

The initial language subset supports `int` and `long` function declarations,
up to six integer parameters, local integer variables, function calls,
assignments, integer literals, arithmetic, comparisons, bitwise and logical
operators, `if`/`else`, `while`, and `return`. It does not yet implement the
C preprocessor, headers, pointers, arrays, strings, global variables, or
standard-library calls.

`make test` builds the host-side compiler, checks NASM output, creates an ELF,
and runs the test program. The test adjusts the OS-specific exit syscall in
the test ELF so it can also execute on the host Linux test runner.

So still in beta idk if it works i tested on my pc and it had bugs lmao
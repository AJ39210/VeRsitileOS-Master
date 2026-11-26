# VeRsitileOS libc

This directory contains the freestanding x86-64 C library used by programs in
VeRsitileOS. Build the shared library with `make -C Kernel libc`. The built
runtime copy is kept at `system/lib/libc.so`, so `make clean` can remove
generated ISO/build directories without deleting it. `make -C Kernel iso`
copies `system/` into the ISO root, placing the library at `/lib/libc.so`.

The current implementation provides a graphics-oriented C/POSIX foundation:
an arena-based allocator, common string and character functions, formatted
I/O, `setjmp`/`longjmp`, read-only stdio/file-descriptor operations, seeking,
process ID, monotonic time helpers, endian conversion macros, and common
case-insensitive string functions. `open`/`fopen` currently support read-only
files; the mounted ISO root is read-only. Anonymous private `mmap`, `munmap`,
and `mprotect` are available. The initial local-socket support includes
`socketpair(AF_UNIX, SOCK_STREAM, ...)` and `poll`; pathname sockets and TCP/
UDP sockets are not implemented yet. This is not a complete POSIX libc:
process creation/wait, signals, threads, general socket operations, metadata/
stat calls, locale, and floating-point formatting are not implemented. The
allocator is currently single-threaded and has a fixed limit of 64
kernel-backed arenas per process.

The kernel ABI is deliberately small. The libc's POSIX-style wrappers only
expose operations that the kernel can currently provide. The file, allocation,
string, stdio, endian, and non-local jump APIs are used by the bundled graphics
libraries. Build them with `make -C Kernel graphics-libs` or as part of
`make -C Kernel libc` and `make -C Kernel iso`. Persistent outputs are stored
in `system/lib/`; libc and graphics-library headers are stored in
`system/include/`. Both the Kernel and libc clean targets leave these packaged
files intact.

The graphics archives are static libraries:

- `libz.a` provides zlib/DEFLATE and gzip-file APIs. Its default stream
  allocator uses the OS libc's `malloc`/`free`; gzip file access uses the
  libc descriptor and seek calls.
- `libpng.a` provides the core PNG read/write APIs and is configured without
  floating-point, simplified-image, stdio convenience, and time-conversion
  features. Applications should use libpng's callback-based I/O API and the
  fixed-point color APIs.
- `libfreetype.a` includes the TrueType/SFNT drivers and monochrome/anti-aliased
  rasterizers. Optional font drivers, compressed-font helpers, environment
  property parsing, and SVG support are disabled.

Applications should include headers from `system/include/` and
`system/include/freetype2/`, and link in dependency order, for example
`-lfreetype -lpng -lz -lc`. The archives are compiled as freestanding x86-64
PIC without SSE or floating-point instructions to match the current user-task
context. The boot-time ELF test writes and decodes a 1x1 PNG in memory, checks
zlib inflate initialization, and initializes FreeType against the OS libc.
Actual image-file decoding and loading/rendering a TrueType font still need
validation with suitable files inside VeRsitileOS.

The kernel ELF loader supports PIE `ET_DYN` executables with one dependency,
`libc.so`, and eagerly applies the supported x86-64 `RELA` relocations. Programs
must not use `PT_INTERP`; the kernel loads the library from `/lib/libc.so`.
Static `ET_EXEC` programs remain supported as well. The ABI and current limits
are exercised by `/bin/elf_test` during QEMU boot.

The native desktop/windowing library is built in `port/libx/`; build it
with `make -C Kernel libx`. It installs `libx.a` and the `X11/` API headers
under `system/`. This is a VeRsitileOS-specific in-process window manager and
drawing/event library, not the X11 wire protocol and not compatible with
upstream Xlib or XCB. Applications currently share the system framebuffer and
input event streams, so a separate X server, cross-process window surfaces,
and isolation between simultaneous graphical applications are not provided.
The older upstream X11 documentation snapshots under `docs/libx/` are not
compiled by the build.

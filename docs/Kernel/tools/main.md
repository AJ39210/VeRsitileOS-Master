# Kernel tools

The tools directory currently contains the host-side OpenGL/X11 configuration app. It runs on the development host; it is not a kernel executable or an OS userspace program.

- [Configuration app](configuration_app/main.md) — GUI for `Version.cfg` and driver selections in `config.cfg`.

The kernel build itself is driven by [`Kernel/Makefile`](../../../Kernel/Makefile) and its included [`Kernel/config.cfg`](../../../Kernel/config.cfg). Run the app with `make -C Kernel cfg` from the repository root, or `make cfg` from the `Kernel` directory.

# Configuration application implementation

[`main.cpp`](../../../../../Kernel/tools/configuration_app/src/main.cpp) contains the host GUI, config parsers, file picker, and regression/self-test modes.

- The Version tab loads/saves `Kernel/Version.cfg`; it validates manifest paths and versions and writes through a temporary file before replacing the config.
- The Config tab loads `DRIVER_ENABLE_<path>` entries from `Kernel/config.cfg`, displays each as an enabled/disabled toggle, and updates the setting using a temporary file and rename.
- `--check` validates the version manifest and driver settings. `--self-test` exercises version save/reload, driver toggling, search focus transfer, and safe removal of a manifest entry.
- X11/GLX and OpenGL provide the host window/rendering. The Makefile under the parent folder defines compile and check commands.

This program edits build configuration but does not itself compile the kernel. The GNU Make build reads `config.cfg`.

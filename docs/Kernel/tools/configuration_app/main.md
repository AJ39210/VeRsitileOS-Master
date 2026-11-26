# Configuration application

The host GUI source is [`src/main.cpp`](../../../../Kernel/tools/configuration_app/src/main.cpp); its host build/test rules are in [`Makefile`](../../../../Kernel/tools/configuration_app/Makefile). It uses X11/GLX and OpenGL and must be run in a graphical host session.

## Tabs

- **Version** reads and edits per-file version entries in [`Kernel/Version.cfg`](../../../../Kernel/Version.cfg), supports search, adding existing files, notes, and confirmed removal of a manifest entry. Removing an entry does not delete the source file.
- **Config** lists `DRIVER_ENABLE_<path>` entries from [`Kernel/config.cfg`](../../../../Kernel/config.cfg). Clicking a row toggles `y`/`n`; the app writes the selection back immediately.

## Commands

- `make -C Kernel cfg` — build if needed and launch the app.
- `make -C Kernel/tools/configuration_app check` — validate the version manifest/build config and run regression self-tests.
- `make -C Kernel/tools/configuration_app clean all` — rebuild the host app.

The app's `--check` and `--self-test` command-line modes are also used by `make check`. The Config tab does not expose arbitrary editing of compiler flags; the config file can be edited directly with a text editor.

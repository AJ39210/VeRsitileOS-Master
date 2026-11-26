# Serial port and boot logging

[`serial.c`](../../../../Kernel/Drivers/serial/serial.c) initializes the serial console, emits characters/strings/numbers, and formats severity-tagged log messages. It also maintains boot-log data with size/read access and implements console output/replay helpers used by kernel and user output.

Serial initialization is the first major step in `kmain()`; failure is fatal because early errors would otherwise have no reliable logging path. Later boot messages include device and memory initialization status. When the framebuffer becomes available, replay shows only the most recent screenful of boot history rather than scrolling the entire retained log onto the display.

API and log-level types: [`Include/serial.h`](../../../../Kernel/Include/serial.h).

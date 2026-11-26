# Kernel public headers

[`Kernel/Include/`](../../../Kernel/Include/) contains shared C interfaces and data structures used by kernel modules. The headers are grouped conceptually as follows:

- **Boot and CPU:** `limine.h`, `gdt.h`, `idt.h`, `isr.h`, `irq.h`, `pic.h`, `apic.h`, `acpi.h`, `timer.h`.
- **Memory and tasks:** `mm.h`, `pmm.h`, `paging.h`, `vmm.h`, `region.h`, `heap.h`, `spinlock.h`, `task.h`, `elf_loader.h`.
- **Storage and filesystems:** `block.h`, `ata_pio.h`, `atapi.h`, `mbr.h`, `vfs.h`, `fat32.h`, `iso9660.h`, `devfs.h`, `procfs.h`, `sysfs.h`.
- **Devices and graphics:** `pci.h`, `xhci.h`, `hid.h`, `keyboard.h`, `ps2.h`, `mouse.h`, `e1000.h`, `network.h`, `intel8265.h`, `virtio_*.h`, `gop.h`, `vesa.h`, `drm.h`, `radeonhd.h`.
- **Diagnostics and ABI:** `serial.h`, `panic.h`, `test.h`, `syscall.h`, `user_syscall.h`, `module.h`.

Headers define interfaces; implementation and current hardware capability belong to their corresponding source modules. In particular, a declared API does not guarantee a complete hardware backend—check the relevant subsystem page.

User/kernel ABI headers are also shared with userland under `libc/include`; update both sides carefully when changing syscall structures or constants.

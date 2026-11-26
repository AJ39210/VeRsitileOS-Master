VeRsitileOS - Added Features

Status:
    XX = built and tested in at least one build or QEMU run ( basicly its tested and it runs well)
    X  = implemented, but incomplete or not fully tested ( bugs may appear as its not a tested build basicly BETA lol)
    ?  = not implemented yet ( will be added in the next snapshoot )
    !  = Decomisioned / Removed
                            
Kernel and userspace
    XX  int 0x80 system-call interface and user-mode syscall wrappers
    XX  Ring 3 tasks, isolated address spaces, task creation and exit
    XX  Preemptive task scheduling and kernel/user memory management
    XX  ELF64 program loading and execution, including programs in /bin
    XX  Dynamic libc loading and support for common x86-64 ELF relocations
    XX  Anonymous mmap, munmap, and mprotect support
    XX  User-space C runtime startup and command arguments
    XX  Kernel version reporting

Libc and libraries
    XX  In-tree freestanding libc with memory, string, stdio, file, time,
        and other basic C runtime functions
    XX  zlib, libpng, and FreeType built as OS libraries
    XX  Libraries packaged under system/lib and included in the ISO
    XX  C ELF test program exercising libc, PNG, zlib, and FreeType

Filesystems and storage
    XX  Read-only root filesystem loaded by Limine initramfs, with ISO9660/ATAPI fallback
    XX  devfs, procfs, and sysfs foundations
    XX  /proc CPU, memory, device, partition, and process information
    XX  Basic /dev devices, including console, null, zero, keyboard, and framebuffer
    XX  MBR and extended-partition scanning
    XX  GPT partition scanning with header and entry-array CRC checks
    XX  FAT32 partition discovery and mounting through the shared block layer
    XX  Temporary in-memory /tmp directory, reset on reboot
    XX  Temporary RAM directories with mkdir and rmdir support
    XX  USB and VirtIO disks connected to the block, partition, and FAT32 layers
    XX  lsblk-style storage visibility and filesystem read tests

Graphics and desktop
    XX  Framebuffer text console using the bundled PSF font
    XX  Boot-log replay on the framebuffer while kernel logs remain on serial
    XX  Colored INFO, ERROR, warning, and success log output
    XX  DRM software framebuffer core and framebuffer device support
    XX  UEFI GOP framebuffer discovery and setup
     !  Desktop environment and graphical terminal mode (removed; console terminal remains)
     !  Desktop menu, terminal window, mouse input, and PNG application icons ( Terminal mode only)
    XX  Separate ELF command programs installed in /bin
    X   Terminal commands including ls, cd, mkdir, rmdir, cat, ping,
        neofetch, and ifconfig
    XX  Console terminal and child-command output capture
    XX  Boot log file exposed at /var/boot_logs/boot.txt for reading
    X   Radeon HD Evergreen/Northern Islands PCI detection and debug reporting;
        this is not a complete accelerated GPU driver

Input and USB
    XX  PCI discovery and device inventory with vendor/device information
    XX  xHCI controller initialization and USB device enumeration
    XX  USB boot-protocol HID keyboard and mouse input
    XX  USB keyboard input verified through the terminal
    XX  USB Mass Storage Bulk-Only Transport and FAT32 USB-drive access
     !   Mouse movement/button handling has less end-to-end testing than keyboard (No Desktop Envrioment yet)
        input; arbitrary HID devices and hotplug recovery are not supported

Networking
    XX  QEMU-compatible Intel e1000 Ethernet driver
    XX  Ethernet, ARP, IPv4, DHCP, UDP, ICMP ping, and DNS lookup
    XX  Network commands and kernel network syscall integration
    XX  Ping and DNS timeout handling fixed so failed requests return
    XX  Legacy VirtIO network driver integrated with the network stack
    XX  QEMU VirtIO network DHCP, gateway ping, and DNS lookup tested
    X   Intel 8265/8275 PCI detection, BAR0 MMIO mapping, bounded DMA-page
        allocation, API-36 firmware TLV/section validation, bounded firmware
        image loading into kernel memory, section inspection, CRC-32
        fingerprinting, image-size/chunk planning, and aligned init/runtime
        chunk staging to the DMA page. Device firmware upload, radio startup,
        scanning, and Wi-Fi packet transport are not implemented or hardware
        validated.

VirtIO
    XX  Legacy and modern VirtIO PCI transport discovery
    XX  VirtIO block device support through the common block layer
    XX  VirtIO GPU 2D scanout backed by the software framebuffer
    XX  VirtIO keyboard and relative mouse input devices
    XX  Boot with VirtIO block, GPU, and input devices; system tests passed
    X   VirtIO input event delivery and disk writes need more direct testing

X11 and POSIX groundwork
    XX  POSIX-style memory mapping APIs and regression tests
    XX  Local stream socketpair and poll support
    ?   X server framebuffer/input backend
    ?   Pathname sockets and the broader POSIX interfaces required by Xorg

Not implemented or not complete
    - Full Radeon, Intel, or NVIDIA graphics drivers
    - Working Wi-Fi networking and nearby-network scanning
    - Full writable filesystem support and persistent /var log storage
    - A complete Linux/POSIX compatibility layer or working Xorg server

Other
    X  Human Interface Device (HID) (Boot-protocol USB keyboard and mouse supported over xHCI polling; tested with QEMU USB devices)


Check more about stuff and things in staging.md ight ? 

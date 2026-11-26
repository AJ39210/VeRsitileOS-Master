# System installer

The `install` command performs a destructive, whole-disk installation. It is included in the live system under `/bin/install`. Run it only when the intended target is a disposable disk or a disk whose contents are backed up.

## Interactive workflow

The command lists detected writable disks, selects the first listed disk by default, and asks which disk to use. Before writing, the user must type the exact string `WIPE <device-name>` (for example, `WIPE vd0`). Any other response cancels the installation.

The kernel installer independently checks the device, request values, and confirmation before changing the disk. This prompt is a safeguard for the normal command, not a security boundary: the OS currently has no user privilege model, so another userspace program can call the install syscall directly.

After confirming, the installer creates the disk layout, installs the BIOS and UEFI boot files, and extracts `/system/install/rootfs.tar` into the ext2 root partition. The archive is generated from `system2/` by `make -C Kernel rootfs`; application sources and headers are excluded, and the live `install` command is not copied into the installed system.

## Disk layout and boot

The installer replaces the disk's partition table with GPT and creates:

| Partition | Starting LBA | Size / contents |
|---|---:|---|
| BIOS boot | 2,048 | 32,768 sectors (16 MiB); Limine BIOS stages |
| EFI System Partition | 34,816 | 262,144 sectors (128 MiB); FAT32, UEFI boot files and Limine configuration |
| System root | 296,960 | Remaining usable disk space; ext2 labeled `VeRsitileOS` |

The BIOS stage is installed in the BIOS boot partition and the disk boot area. The EFI partition receives the fallback `EFI/BOOT/BOOTX64.EFI`, the kernel, Limine support files, and a generated Limine configuration. That configuration passes `root=VeRsitileOS`; on boot, the kernel uses that explicit option and the ext2 label to select the installed system root.

The disk must have at least 131,072 sectors available for the ext2 root in addition to the reserved boot areas and GPT metadata. The ext2 filesystem's supported size limit also applies.

## First-time setup

After installation, boot the installed system and run `setup` from the terminal. Setup asks for a computer name, username, and local domain (it appends `.dom` when omitted). It detects the first ELF executable under `/etc/xsession`; when none is present, it records `Unknown / terminal`.

Once confirmed, setup writes the hostname to `/etc/system/host`, the domain to `/etc/system/domain`, and the selected desktop to `/etc/system/desktop`. It creates `/etc/system/users/<username>` and `/home/<username>`. The final prompt can remove `/bin/setup` from the installed ext2 filesystem.

## Host-side validation

The installer workflow test checks confirmation rejection, partition placement, filesystem setup, rootfs archive extraction, UEFI files/configuration, and BIOS stage placement:

```sh
make -C Kernel test-installer
```

The filesystem and partition tests can be run together with:

```sh
make -C Kernel test-ext2 test-gpt test-fat32 test-installer
```

These tests do not replace boot testing. A complete BIOS and UEFI boot should be tested under a VM with a disposable disk image; do not validate installation against a real disk.

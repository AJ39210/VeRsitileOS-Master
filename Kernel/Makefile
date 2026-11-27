
include config.cfg

CARGO ?= cargo
RUST_TARGET ?= x86_64-unknown-none
PYTHON ?= python3
OBJCOPY ?= objcopy
MODULE_SIGNING_KEY ?= $(HOME)/.config/versitileos/module-signing-key.pem
MODULE_SIGNING_PUBLIC_KEY = security/cert/module_signing_public_key.bin
MODULE_SIGNING_KEY_DEPENDENCY = $(wildcard $(MODULE_SIGNING_KEY))

ENABLED_DRIVER_SOURCES = $(foreach source,$(DRIVER_SOURCES),$(if $(filter y,$(DRIVER_ENABLE_$(source))),$(source)))
KERNEL_MM_C_SOURCES = \
	Kernel/mm/pmm.c \
	Kernel/mm/vmm.c \
	Kernel/mm/paging.c \
	Kernel/mm/mm.c \
	Kernel/mm/sync/spinlock.c \
	Kernel/mm/region/region.c \
	Kernel/mm/heap/heap.c
ifeq ($(MM_IMPLEMENTATION),1)
KERNEL_MM_SELECTED_C_SOURCES = $(KERNEL_MM_C_SOURCES)
else ifeq ($(MM_IMPLEMENTATION),2)
KERNEL_MM_SELECTED_C_SOURCES =
else
$(error MM_IMPLEMENTATION must be 1 (C) or 2 (Rust), got '$(MM_IMPLEMENTATION)')
endif
KERNEL_SOURCES = $(KERNEL_C_SOURCES) $(KERNEL_MM_SELECTED_C_SOURCES) $(ENABLED_DRIVER_SOURCES)
C_OBJECTS = $(KERNEL_SOURCES:%.c=$(BUILD)/%.o)
ASM_OBJECTS = $(ASM_SOURCES:%.asm=$(BUILD)/asm/%.o)
RUST_LIBRARIES = $(BUILD)/rust/kernel-mm$(MM_IMPLEMENTATION).a
RUST_SOURCES = $(shell find rust/src security -type f \( -name '*.rs' -o -name '*.bin' \))
RUST_DEPENDENCIES = $(filter-out $(KERNEL_RUST_SOURCES),$(RUST_SOURCES)) \
	rust/Cargo.toml rust/Cargo.lock
PANIC_IMAGE_SOURCE = tools/panic.png
PANIC_IMAGE_DATA = $(BUILD)/panic_background.rgb
PANIC_IMAGE_OBJECT = $(BUILD)/panic_background.o
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS) $(RUST_LIBRARIES) $(PANIC_IMAGE_OBJECT)
MODULE_INSTALL_DIR = $(ROOT)/system/lib/modules/$(KERNEL_VERSION)
RADEONHD_MODULE_BUILD = $(BUILD)/modules/radeonhd.ko
INTEL_HD2000_MODULE_BUILD = $(BUILD)/modules/intel_hd2000.ko
NVIDIA_8400GS_MODULE_BUILD = $(BUILD)/modules/nvidia_8400gs.ko
E1000_MODULE_BUILD = $(BUILD)/modules/e1000.ko
RTL810X_MODULE_BUILD = $(BUILD)/modules/realtek_rtl810x.ko
VIRTIO_NET_MODULE_BUILD = $(BUILD)/modules/virtio_net.ko
VIRTIO_GPU_MODULE_BUILD = $(BUILD)/modules/virtio_gpu.ko
MODULE_BUILDS = \
	$(RADEONHD_MODULE_BUILD) \
	$(INTEL_HD2000_MODULE_BUILD) \
	$(NVIDIA_8400GS_MODULE_BUILD) \
	$(E1000_MODULE_BUILD) \
	$(RTL810X_MODULE_BUILD) \
	$(VIRTIO_NET_MODULE_BUILD) \
	$(VIRTIO_GPU_MODULE_BUILD)
MODULE_OBJECTS = \
	$(BUILD)/modules/radeonhd_module.o \
	$(BUILD)/modules/intel_hd2000_module.o \
	$(BUILD)/modules/nvidia_8400gs_module.o \
	$(BUILD)/drm/amd/amd_drm.o \
	$(BUILD)/drm/intel/intel_drm.o \
	$(BUILD)/drm/nvidia/nvidia_drm.o \
	$(BUILD)/modules/e1000_module.o \
	$(BUILD)/Drivers/Ethernet/e1000/e1000.o \
	$(BUILD)/modules/rtl810x_module.o \
	$(BUILD)/Drivers/Ethernet/Realtek/rtl810x.o \
	$(BUILD)/Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.o \
	$(BUILD)/Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.o \
	$(BUILD)/modules/virtio_net_module.o \
	$(BUILD)/Drivers/Virtio/virtio_net.o \
	$(BUILD)/modules/virtio_gpu_module.o \
	$(BUILD)/Drivers/Virtio/virtio_gpu.o \
	$(BUILD)/gpu/amd/RadeonHD/radeonhd.o \
	$(BUILD)/gpu/intel/HDGraphics2000/intel_hd2000.o \
	$(BUILD)/gpu/nvidia/GeForce8400GS/nvidia_8400gs.o
MODULE_INSTALLS = \
	$(MODULE_INSTALL_DIR)/radeonhd.ko \
	$(MODULE_INSTALL_DIR)/intel_hd2000.ko \
	$(MODULE_INSTALL_DIR)/nvidia_8400gs.ko \
	$(MODULE_INSTALL_DIR)/e1000.ko \
	$(MODULE_INSTALL_DIR)/realtek_rtl810x.ko \
	$(MODULE_INSTALL_DIR)/virtio_net.ko \
	$(MODULE_INSTALL_DIR)/virtio_gpu.ko
-include $(C_OBJECTS:.o=.d)
-include $(MODULE_OBJECTS:.o=.d)
KERNEL_CONFIG_STAMP = $(BUILD)/kernel-config.stamp
KERNEL_CONFIG_SIGNATURE_FILE = $(BUILD)/kernel-config.signature
CONFIG_SIGNATURE = $(shell sed '/^[[:space:]]*DRIVER_ENABLE_/d' config.cfg | cksum)
ACTIVE_CONFIG_SIGNATURE = $(shell cat $(KERNEL_CONFIG_SIGNATURE_FILE) 2>/dev/null)

.PHONY: all kernel modules module-signing-key libc graphics-libs libx doom rootfs iso run run-debug run-disk cfg clean clr help test-ext2 test-gpt test-fat32 test-installer test-rtl810x test-ar5007eg test-libx
.PHONY: FORCE_CONFIG_SIGNATURE

HOST_CC ?= gcc

all: iso

# Build kernel
kernel: $(BUILD)/kernel.elf

modules: $(MODULE_INSTALLS)

module-signing-key:
	$(PYTHON) tools/module_signing.py keygen --replace-trusted-key

$(BUILD)/kernel.elf: $(OBJECTS) $(KERNEL_LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

$(PANIC_IMAGE_DATA): $(PANIC_IMAGE_SOURCE) tools/panic_png_to_rgb.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/panic_png_to_rgb.py $(PANIC_IMAGE_SOURCE) $@

$(PANIC_IMAGE_OBJECT): $(PANIC_IMAGE_DATA)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata.panic_background,alloc,load,readonly,data,contents \
		--redefine-sym _binary_build_panic_background_rgb_start=panic_background_start \
		--redefine-sym _binary_build_panic_background_rgb_end=panic_background_end \
		$< $@

# Build optional GPU probe implementations as relocatable ELF modules.
$(BUILD)/modules/radeonhd_module.o: Kernel/modules/radeonhd_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/intel_hd2000_module.o: Kernel/modules/intel_hd2000_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/nvidia_8400gs_module.o: Kernel/modules/nvidia_8400gs_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/e1000_module.o: Kernel/modules/e1000_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Ethernet/e1000/e1000.o: Drivers/Ethernet/e1000/e1000.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/rtl810x_module.o: Kernel/modules/rtl810x_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Ethernet/Realtek/rtl810x.o: Drivers/Ethernet/Realtek/rtl810x.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.o: Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.o: Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/virtio_net_module.o: Kernel/modules/virtio_net_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Virtio/virtio_net.o: Drivers/Virtio/virtio_net.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/modules/virtio_gpu_module.o: Kernel/modules/virtio_gpu_module.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/Drivers/Virtio/virtio_gpu.o: Drivers/Virtio/virtio_gpu.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/gpu/amd/RadeonHD/radeonhd.o: gpu/amd/RadeonHD/radeonhd.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/drm/amd/amd_drm.o: drm/amd/amd_drm.c Include/amd_drm.h $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/drm/intel/intel_drm.o: drm/intel/intel_drm.c Include/intel_drm.h $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/drm/nvidia/nvidia_drm.o: drm/nvidia/nvidia_drm.c Include/nvidia_drm.h $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/gpu/intel/HDGraphics2000/intel_hd2000.o: gpu/intel/HDGraphics2000/intel_hd2000.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(BUILD)/gpu/nvidia/GeForce8400GS/nvidia_8400gs.o: gpu/nvidia/GeForce8400GS/nvidia_8400gs.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables -c $< -o $@

$(RADEONHD_MODULE_BUILD): $(BUILD)/modules/radeonhd_module.o $(BUILD)/gpu/amd/RadeonHD/radeonhd.o $(BUILD)/drm/amd/amd_drm.o
	$(LD) -r -o $@ $^

$(INTEL_HD2000_MODULE_BUILD): $(BUILD)/modules/intel_hd2000_module.o $(BUILD)/gpu/intel/HDGraphics2000/intel_hd2000.o $(BUILD)/drm/intel/intel_drm.o
	$(LD) -r -o $@ $^

$(NVIDIA_8400GS_MODULE_BUILD): $(BUILD)/modules/nvidia_8400gs_module.o $(BUILD)/gpu/nvidia/GeForce8400GS/nvidia_8400gs.o $(BUILD)/drm/nvidia/nvidia_drm.o
	$(LD) -r -o $@ $^

$(E1000_MODULE_BUILD): $(BUILD)/modules/e1000_module.o $(BUILD)/Drivers/Ethernet/e1000/e1000.o
	$(LD) -r -o $@ $^

$(RTL810X_MODULE_BUILD): $(BUILD)/modules/rtl810x_module.o \
	$(BUILD)/Drivers/Ethernet/Realtek/rtl810x.o \
	$(BUILD)/Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.o \
	$(BUILD)/Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.o
	$(LD) -r -o $@ $^

$(VIRTIO_NET_MODULE_BUILD): $(BUILD)/modules/virtio_net_module.o $(BUILD)/Drivers/Virtio/virtio_net.o
	$(LD) -r -o $@ $^

$(VIRTIO_GPU_MODULE_BUILD): $(BUILD)/modules/virtio_gpu_module.o $(BUILD)/Drivers/Virtio/virtio_gpu.o
	$(LD) -r -o $@ $^

$(MODULE_INSTALL_DIR)/radeonhd.ko: $(BUILD)/modules/radeonhd.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/intel_hd2000.ko: $(BUILD)/modules/intel_hd2000.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/nvidia_8400gs.ko: $(BUILD)/modules/nvidia_8400gs.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/e1000.ko: $(BUILD)/modules/e1000.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/realtek_rtl810x.ko: $(BUILD)/modules/realtek_rtl810x.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/virtio_net.ko: $(BUILD)/modules/virtio_net.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(MODULE_INSTALL_DIR)/virtio_gpu.ko: $(BUILD)/modules/virtio_gpu.ko.signed
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/modules/%.ko.signed: $(BUILD)/modules/%.ko \
	tools/module_signing.py $(MODULE_SIGNING_PUBLIC_KEY) \
	$(MODULE_SIGNING_KEY_DEPENDENCY)
	@test -r "$(MODULE_SIGNING_KEY)" || { \
		echo "Missing module signing key: $(MODULE_SIGNING_KEY)" >&2; \
		echo "Run 'make module-signing-key' or set MODULE_SIGNING_KEY." >&2; \
		exit 1; \
	}
	$(PYTHON) tools/module_signing.py sign \
		--key "$(MODULE_SIGNING_KEY)" \
		--trusted-key "$(MODULE_SIGNING_PUBLIC_KEY)" \
		$< $@

# Compile C files
$(BUILD)/%.o: %.c $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/Kernel/kmain.o: Version.cfg

cfg:
	$(MAKE) -C tools/configuration_app run

$(BUILD)/asm/%.o: %.asm $(KERNEL_CONFIG_STAMP)
	mkdir -p $(dir $@)
	$(NASM) -f $(NASM_FORMAT) $< -o $@

$(BUILD)/rust/kernel-mm1.a: $(KERNEL_RUST_SOURCES) $(RUST_DEPENDENCIES)
	mkdir -p $(dir $@)
	CARGO_CFG_CURVE25519_DALEK_BACKEND=serial \
		RUSTFLAGS="-C panic=abort -C opt-level=2 -C relocation-model=static \
		-C force-frame-pointers=yes -C code-model=kernel -C no-redzone=yes \
		-C force-unwind-tables=no -C target-feature=-sse,-sse2" \
		CARGO_TARGET_DIR=$(BUILD)/cargo-mm1-serial \
		$(CARGO) build --manifest-path rust/Cargo.toml --release \
		--locked --target=$(RUST_TARGET) --no-default-features
	cp $(BUILD)/cargo-mm1-serial/$(RUST_TARGET)/release/libversitile_kernel.a $@

$(BUILD)/rust/kernel-mm2.a: $(KERNEL_RUST_SOURCES) $(RUST_DEPENDENCIES)
	mkdir -p $(dir $@)
	CARGO_CFG_CURVE25519_DALEK_BACKEND=serial \
		RUSTFLAGS="-C panic=abort -C opt-level=2 -C relocation-model=static \
		-C force-frame-pointers=yes -C code-model=kernel -C no-redzone=yes \
		-C force-unwind-tables=no -C target-feature=-sse,-sse2" \
		CARGO_TARGET_DIR=$(BUILD)/cargo-mm2-serial \
		$(CARGO) build --manifest-path rust/Cargo.toml --release \
		--locked --target=$(RUST_TARGET) --no-default-features --features rust-mm
	cp $(BUILD)/cargo-mm2-serial/$(RUST_TARGET)/release/libversitile_kernel.a $@

ifneq ($(strip $(CONFIG_SIGNATURE)),$(strip $(ACTIVE_CONFIG_SIGNATURE)))
$(KERNEL_CONFIG_STAMP): FORCE_CONFIG_SIGNATURE
endif

$(KERNEL_CONFIG_STAMP):
	mkdir -p $(dir $@)
	@printf '%s\n' '$(CONFIG_SIGNATURE)' > $(KERNEL_CONFIG_SIGNATURE_FILE).tmp
	@mv $(KERNEL_CONFIG_SIGNATURE_FILE).tmp $(KERNEL_CONFIG_SIGNATURE_FILE)
	@touch $@

MISSING_GRAPHICS_OUTPUTS := $(filter-out $(wildcard $(GRAPHICS_LIB_OUTPUTS)),$(GRAPHICS_LIB_OUTPUTS))
ifneq ($(strip $(MISSING_GRAPHICS_OUTPUTS)),)
.PHONY: FORCE_GRAPHICS_LIBS
$(GRAPHICS_STAMP): FORCE_GRAPHICS_LIBS
endif

$(GRAPHICS_STAMP): $(LIBC_ROOT)/Makefile $(GRAPHICS_BUILD_INPUTS)
	$(MAKE) -C $(LIBC_ROOT) all
	mkdir -p $(dir $@)
	touch $@

graphics-libs: $(GRAPHICS_STAMP)

libx:
	$(MAKE) -C $(LIBX_ROOT) ROOT=$(abspath $(ROOT)) all

doom: $(USER_DOOM)

rootfs: kernel modules $(SYSTEM2_TERMINAL) $(SYSTEM2_INIT) \
	$(SYSTEM2_COMMANDS) $(SYSTEM2_KILO) $(SYSTEM2_VCC) $(SYSTEM2_DOOM) \
	$(SYSTEM2_SETUP) $(SYSTEM_LIBC) graphics-libs libx
	mkdir -p $(SYSTEM2_ROOT)/boot/limine \
		$(SYSTEM2_ROOT)/usr/share/doom \
		$(SYSTEM2_ROOT)/etc/system/users \
		$(dir $(ROOTFS_ARCHIVE))
	cp $(BUILD)/kernel.elf $(SYSTEM2_ROOT)/boot/kernel.elf
	cp $(LIMINE)/limine-bios.sys $(SYSTEM2_ROOT)/boot/limine/
	cp -a $(ROOT)/system/lib/. $(SYSTEM2_ROOT)/lib/
	cp $(DOOM_IWADS) $(SYSTEM2_ROOT)/usr/share/doom/
	cp $(DOOM_IWAD_LICENSE) \
		$(SYSTEM2_ROOT)/usr/share/doom/COPYING.FREEDOOM
	tar --format=ustar --create --file=$(ROOTFS_ARCHIVE) \
		--directory=$(SYSTEM2_ROOT) \
		--exclude=./src --exclude=./include \
		--exclude=./bin/install .

test-ext2:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include tests/ext2_test.c fs/ext2/ext2.c \
		-o $(BUILD)/tests/ext2_test
	$(BUILD)/tests/ext2_test $(BUILD)/tests/ext2.img
	e2fsck -fn $(BUILD)/tests/ext2.img

test-gpt:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include tests/gpt_test.c fs/mbr.c \
		-o $(BUILD)/tests/gpt_test
	$(BUILD)/tests/gpt_test

test-fat32:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include tests/fat32_test.c fs/fat32/fat32.c \
		-o $(BUILD)/tests/fat32_test
	$(BUILD)/tests/fat32_test $(BUILD)/tests/fat32.img
	fsck.fat -n $(BUILD)/tests/fat32.img

test-installer:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include -I $(LIBC_INCLUDE) \
		tests/installer_test.c fs/installer.c \
		-o $(BUILD)/tests/installer_test
	$(BUILD)/tests/installer_test

test-rtl810x:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include tests/rtl810x_match_test.c \
		Drivers/Ethernet/Realtek/RTL8102E/rtl8102e.c \
		Drivers/Ethernet/Realtek/RTL8103E/rtl8103e.c \
		-o $(BUILD)/tests/rtl810x_match_test
	$(BUILD)/tests/rtl810x_match_test

test-ar5007eg:
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-I Include tests/ar5007eg_match_test.c \
		-o $(BUILD)/tests/ar5007eg_match_test
	$(BUILD)/tests/ar5007eg_match_test

test-libx: libx
	mkdir -p $(BUILD)/tests
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror \
		-I tests/libx_mock/include \
		-I ../port/libx/include -I $(LIBC_INCLUDE) \
		-fsyntax-only ../port/libx/src/xlib.c ../port/libx/src/xwidgets.c
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror \
		-I ../port/libx/include tests/libx_api_test.c \
		-Wl,--whole-archive $(ROOT)/system/lib/libx.a \
		-Wl,--no-whole-archive \
		-o $(BUILD)/tests/libx_api_test
	$(BUILD)/tests/libx_api_test
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror \
		-I tests/libx_mock/include -I ../port/libx/include \
		-I $(LIBC_INCLUDE) \
		tests/libx_runtime_test.c \
		../port/libx/src/xlib.c ../port/libx/src/xwidgets.c \
		-o $(BUILD)/tests/libx_runtime_test
	$(BUILD)/tests/libx_runtime_test

# Build bootable ISO
iso: rootfs kernel modules $(USER_ELF) $(USER_TERMINAL) $(USER_SYSTEMD) \
	$(USER_SETUP) \
	$(USER_COMMANDS) $(USER_KILO) $(USER_VCC) \
	$(USER_DOOM) $(SYSTEM_LIBC) graphics-libs libx
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine
	mkdir -p $(ISO_ROOT)/EFI/BOOT

	cp $(BUILD)/kernel.elf $(ISO_ROOT)/boot/kernel.elf

	cp $(LIMINE)/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin $(ISO_ROOT)/boot/
	cp $(LIMINE)/limine-uefi-cd.bin $(ISO_ROOT)/boot/
	cp $(LIMINE)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/

	cp $(LIMINE)/limine.conf $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/versitile-wallpaper.png $(ISO_ROOT)/boot/limine/
	cp -a $(ROOT)/system/. $(ISO_ROOT)/
	mkdir -p $(ISO_ROOT)/system/install
	cp $(ROOTFS_ARCHIVE) $(ISO_ROOT)/system/install/rootfs.tar
	mkdir -p $(ISO_ROOT)/bin
	cp $(USER_ELF) $(ISO_ROOT)/bin/elf_test
	cp $(USER_TERMINAL) $(ISO_ROOT)/bin/terminal
	cp $(USER_KILO) $(ISO_ROOT)/bin/kilo
	cp $(USER_VCC) $(ISO_ROOT)/bin/vcc
	cp $(USER_DOOM) $(ISO_ROOT)/bin/doom
	mkdir -p $(DOOM_DATA_DIR)
	cp $(DOOM_IWADS) $(DOOM_DATA_DIR)/
	cp $(DOOM_IWAD_LICENSE) $(DOOM_DATA_DIR)/COPYING.FREEDOOM

	tar --format=ustar --create --file=$(ISO_ROOT)/boot/system-root.tar \
		--directory=$(ISO_ROOT) --exclude=./boot/system-root.tar .

	xorriso -as mkisofs \
		-R -r -J \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part \
		--efi-boot-image \
		--protective-msdos-label \
		$(ISO_ROOT) \
		-o $(ISO)

	$(LIMINE)/limine bios-install $(ISO)

	@echo "VeRsitileOS ISO created: $(ISO)"

libc: $(SYSTEM_LIBC) graphics-libs

$(SYSTEM_LIBC): $(LIBC_SHARED)
	mkdir -p $(dir $@)
	cp $(LIBC_SHARED) $@

$(BUILD)/libc/%.o: $(LIBC_ROOT)/src/%.c $(LIBC_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

$(USER_CRT0): $(LIBC_ROOT)/crt0.asm
	mkdir -p $(dir $@)
	$(NASM) -f $(NASM_FORMAT) $< -o $@

$(LIBC_SHARED): $(LIBC_OBJECTS) $(LIBC_ASM_OBJECTS)
	$(CC) $(LIBC_LDFLAGS) -o $@ $(LIBC_OBJECTS) $(LIBC_ASM_OBJECTS)

$(BUILD)/libc/setjmp.o: $(LIBC_ROOT)/src/setjmp.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USER_ELF): $(USER_ELF_SOURCE) $(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS) \
	$(GRAPHICS_STAMP)
	mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_ELF_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc \
		-L$(ROOT)/system/lib $(USER_ELF_LIBS)

$(USER_TERMINAL): $(USER_TERMINAL_SOURCE) $(USER_TERMINAL_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(USER_CFLAGS) $(USER_TERMINAL_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc \
		-L$(ROOT)/system/lib $(USER_TERMINAL_LIBS)

$(USER_KILO): $(USER_KILO_SOURCE) $(USER_KILO_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $(USER_KILO_SOURCE) \
		-L$(BUILD)/libc \
		-L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

$(USER_VCC): $(USER_VCC_SOURCE) $(USER_VCC_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $(USER_VCC_SOURCE) \
		-L$(BUILD)/libc \
		-L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

$(USER_DOOM): $(DOOM_SOURCES) $(DOOM_PLATFORM_SOURCES) $(DOOM_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(DOOM_CFLAGS) \
		-o $@ $(USER_CRT0) $(DOOM_SOURCES) $(DOOM_PLATFORM_SOURCES) \
		-L$(BUILD)/libc -L$(ROOT)/system/lib -lc

$(USER_SYSTEMD): $(USER_SYSTEMD_SOURCE) $(USER_SYSTEMD_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $(USER_SYSTEMD_SOURCE) \
		-L$(BUILD)/libc \
		-L$(ROOT)/system/lib $(USER_SYSTEMD_LIBS)

$(USER_SETUP): $(USER_SETUP_SOURCE) $(USER_TERMINAL_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib -lc

$(SYSTEM2_TERMINAL): $(SYSTEM2_ROOT)/src/terminal.c \
	$(USER_TERMINAL_HEADERS) $(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_TERMINAL_LIBS)

$(SYSTEM2_INIT): $(SYSTEM2_ROOT)/src/systemd/systemd.c \
	$(USER_SYSTEMD_HEADERS) $(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_SYSTEMD_LIBS)

$(SYSTEM2_COMMANDS): $(SYSTEM2_BIN)/%: $(SYSTEM2_COMMAND_SOURCE) \
	$(USER_COMMAND_HEADERS) $(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(COMMAND_CFLAGS) $(COMMAND_DEFINE_$*) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

$(SYSTEM2_KILO): $(USER_KILO_SOURCE) $(USER_KILO_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $(USER_KILO_SOURCE) \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

$(SYSTEM2_VCC): $(USER_VCC_SOURCE) $(USER_VCC_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $(USER_VCC_SOURCE) \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

$(SYSTEM2_DOOM): $(DOOM_SOURCES) $(DOOM_PLATFORM_SOURCES) $(DOOM_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(DOOM_CFLAGS) \
		-o $@ $(USER_CRT0) $(DOOM_SOURCES) $(DOOM_PLATFORM_SOURCES) \
		-L$(BUILD)/libc -L$(ROOT)/system/lib -lc

$(SYSTEM2_SETUP): $(USER_SETUP_SOURCE) $(USER_TERMINAL_HEADERS) \
	$(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM2_BIN)
	$(CC) $(USER_CFLAGS) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib -lc

$(USER_COMMANDS): $(SYSTEM_BIN)/%: $(USER_COMMAND_SOURCE) \
	$(USER_COMMAND_HEADERS) $(USER_CRT0) $(LIBC_SHARED) $(LIBC_HEADERS)
	mkdir -p $(SYSTEM_BIN)
	$(CC) $(COMMAND_CFLAGS) $(COMMAND_DEFINE_$*) \
		-o $@ $(USER_CRT0) $< \
		-L$(BUILD)/libc -L$(ROOT)/system/lib $(USER_COMMAND_LIBS)

# Normal QEMU run
run: iso
	qemu-system-x86_64 \
		-m 1G \
		-cdrom $(ISO) \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-boot d \
		-no-reboot \
		-no-shutdown

# QEMU with debug logs and graphical display
run-debug: iso
	qemu-system-x86_64 \
		-m 1G \
		-cdrom $(ISO) \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-boot d \
		-display gtk \
		-monitor none \
		-serial file:log.txt \
		-no-reboot \
		-no-shutdown \
		-d int,cpu_reset,guest_errors \
		-D /dev/stderr 

run-disk: iso
	@if [ ! -f disk.img ]; then \
		qemu-img create -f raw disk.img 1G; \
	fi
	qemu-system-x86_64 \
		-m 1G \
		-cdrom $(ISO) \
		-drive file=disk.img,format=raw,if=ide,snapshot=off \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-boot order=d \
		-no-reboot \
		-no-shutdown

# Clean build files
clean:
	rm -rf $(BUILD)
	rm -f $(ISO)
	rm -rf $(ISO_ROOT)

clr: clean

# Help
help:
	@echo "VeRsitileOS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make             Build bootable ISO"
	@echo "  make kernel      Build kernel ELF"
	@echo "  make libc        Build shared libc"
	@echo "  make graphics-libs Build zlib, libpng, and FreeType"
	@echo "  make libx          Build the native windowing library"
	@echo "  make test-libx     Check the native windowing API and implementation"
	@echo "  make doom       Build the Doom executable"
	@echo "  make cfg         Open Kernel configuration app"
	@echo "  make iso         Build bootable ISO"
	@echo "  make run         Build and run in QEMU"
	@echo "  make run-debug   Run QEMU with debug logs"
	@echo "  make run-disk    Boot the ISO with a persistent Kernel/disk.img"
	@echo "  make clean       Remove build files and ISO"
	@echo "  make clr         Alias for clean"
	@echo "  make help        Show this help"

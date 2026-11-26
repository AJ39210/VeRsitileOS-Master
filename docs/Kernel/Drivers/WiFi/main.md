# Intel 8265/8275 Wi-Fi foundation

## Qualcomm Atheros AR5007EG / AR2425

[`ar5007eg.c`](../../../../Kernel/Drivers/WiFi/ar5007eg.c) recognizes PCI
device `168c:001c`, validates and maps BAR0, and exposes the discovered device
and mapped resource to later driver work. It does not program radio registers,
enable bus mastering, transmit or receive frames, or configure the radio. There
is no scanning, association, WPA2, DHCP integration, or `iwctl`-style command
yet, so this probe alone cannot connect to a network. Hardware validation
requires an AR5007EG/AR2425 machine; QEMU does not emulate this adapter.

[`intel8265.c`](../../../../Kernel/Drivers/WiFi/intel8265.c) matches Intel PCI device `8086:24FD` (8265/8275 family), records matching devices, and reports the number detected. For the first matching device it validates/sizes BAR0, maps its bounded MMIO region, enables PCI memory decoding and bus mastering, and allocates one contiguous 4 KiB DMA page with a 36-bit address check. Boot then reads the firmware image at `/firmware/iwlwifi-8265-36.ucode` through the VFS into bounded kernel-heap memory. In the system tree, that path corresponds to `system/firmware/iwlwifi-8265-36.ucode`.

The loader requires firmware API version 36, validates the container header/build fields, every record boundary, and four-byte TLV/payload sizing, and requires nonempty runtime and init section records (TLV types 19 and 20). It counts runtime, init, and WoWLAN section records for diagnostics. Section records are decoded as a 32-bit device destination followed by section bytes; the parser rejects empty sections and address-range overflow. It preserves destination values as firmware metadata, including special values that are not word-aligned, rather than assuming every destination is a directly writable device address. Image-specific helpers enumerate init or runtime sections without including WoWLAN records and copy bounded payload chunks together with their destination addresses. The cursor-based `intel8265_firmware_copy_next_image_chunk()` and `intel8265_firmware_stage_next_image_chunk()` APIs walk an entire selected image section-by-section, maintaining aligned offsets and reporting when all chunks are staged. `intel8265_firmware_stage_image_chunk()` remains available for explicitly indexed access. Generic TLV access/copy helpers remain available for other records. These APIs prepare section bytes only; hardware transfer code must interpret and validate destinations before programming the card.

The image is read into kernel memory and validated; init/runtime section payload chunks (without destination-address headers) can now be selected and staged in word-sized blocks in the allocated DMA page, but no bytes are sent to the device. PCI/MMIO/DMA work is resource preparation only: no device registers are programmed, the allocated page is not yet a hardware descriptor/ring, and no DMA transaction is started. Firmware boot, device-specific DMA queues/interrupt handling, 802.11 scanning/authentication/association, and packet transport remain unimplemented. Therefore the general network stack continues to use VirtIO or e1000, and this work does not yet discover nearby networks.

The firmware image is not included in the repository. Use a legally obtained, device-compatible Intel firmware blob. Do not treat a zero-length, oversized, missing, or partially read image as ready for hardware upload.

`intel8265_firmware_image_plan()` reports an init/runtime image's payload size and exact number of aligned chunks for a chosen buffer capacity. It rejects unsupported image types and capacities too small or not word-aligned, helping future transfer code size buffers and track progress before interacting with hardware.

The firmware inspection result includes aggregate payload byte totals for runtime, init, and WoWLAN sections, plus a CRC-32 fingerprint of the full container. The loader reports runtime/init totals, firmware build, container size, and CRC-32 when it validates the image. `intel8265_firmware_crc32()` also computes a bounded CRC-32 for external checksum comparisons; internal TLV and section lookups perform structural validation without repeatedly hashing the entire firmware during chunk staging. CRC detects accidental changes only when compared with a trusted expected value; it is not cryptographic authentication and does not establish firmware provenance.

API: [`Include/intel8265.h`](../../../../Kernel/Include/intel8265.h).

## Manifest versions

The current entries in [`Kernel/Version.cfg`](../../../../Kernel/Version.cfg) are:

- `Drivers/WiFi/intel8265.c` — `1.1.5`
- `Include/intel8265.h` — `1.1.4`
- `Kernel/test.c` — `1.1.3`

These are per-file manifest versions, not the overall kernel release. The manifest currently identifies the kernel as `5.0.7`.

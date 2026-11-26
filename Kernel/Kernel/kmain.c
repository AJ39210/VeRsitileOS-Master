
#include <stdint.h>
#include <stdbool.h>

#include "limine.h"

#include "serial.h"
#include "mm.h"
#include "pmm.h"
#include "test.h"

#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "apic.h"
#include "pic.h"
#include "irq.h"
#include "timer.h"
#include "syscall.h"
#include "task.h"

#include "keyboard.h"
#include "ps2.h"
#include "mouse.h"
#include "vesa.h"
#include "gop.h"
#include "drm.h"
#include "pci.h"
#include "xhci.h"
#include "hid.h"
#include "intel8265.h"
#include "ar5007eg.h"
#include "vfs.h"
#include "network.h"
#include "ata_pio.h"
#include "mbr.h"
#include "virtio_pci.h"
#include "virtio_blk.h"
#include "virtio_input.h"

const char kernel_version[] = KERNEL_VERSION "\n";

/* ---------------------------------------------------------
   LIMINE BASE REVISION
--------------------------------------------------------- */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] =
    LIMINE_BASE_REVISION(4);

/* ---------------------------------------------------------
   LIMINE REQUESTS
--------------------------------------------------------- */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0
};

static struct limine_internal_module initramfs_module = {
    .path = "system-root.tar",
    .string = "rootfs",
    .flags = 0
};

static struct limine_internal_module *initramfs_modules[] = {
    &initramfs_module
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1,
    .internal_module_count = 1,
    .internal_modules = initramfs_modules
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request
    executable_cmdline_request = {
        .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
        .revision = 0
    };

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] =
    LIMINE_REQUESTS_END_MARKER;

/* ---------------------------------------------------------
   BASIC HELPERS
--------------------------------------------------------- */

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* ---------------------------------------------------------
   MEMORY MAP
--------------------------------------------------------- */

static void print_memory_map(void)
{
    struct limine_memmap_response *memmap =
        memmap_request.response;

    if (!memmap)
        return;

    serial_log(SERIAL_LOG_INFO, "Memory map:");

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry =
            memmap->entries[i];

        serial_write("  Base: ");
        serial_write_hex(entry->base);

        serial_write(" Length: ");
        serial_write_hex(entry->length);

        serial_write(" Type: ");
        serial_write_dec(entry->type);

        serial_putc('\n');
    }
}

/* ---------------------------------------------------------
   PHYSICAL MEMORY STATISTICS
--------------------------------------------------------- */

static void print_pmm_statistics(void)
{
    serial_log(SERIAL_LOG_INFO, "Physical memory statistics:");

    serial_write("  Total memory: ");
    serial_write_dec(pmm_get_total_memory() / 1024 / 1024);
    serial_write(" MiB\n");

    serial_write("  Used memory:  ");
    serial_write_dec(pmm_get_used_memory() / 1024 / 1024);
    serial_write(" MiB\n");

    serial_write("  Free memory:  ");
    serial_write_dec(pmm_get_free_memory() / 1024 / 1024);
    serial_write(" MiB\n");

    serial_write("  Total pages:  ");
    serial_write_dec(pmm_get_total_pages());
    serial_putc('\n');

    serial_write("  Free pages:   ");
    serial_write_dec(pmm_get_free_pages());
    serial_putc('\n');
}

static bool load_system_font(void)
{
    static uint8_t font_data[65536];
    vfs_file_t font_file;
    if (vfs_open("/fonts/terminus.psf", &font_file) < 0 ||
        font_file.size > sizeof(font_data))
        return false;

    int64_t result = vfs_read(
        &font_file,
        font_data,
        font_file.size
    );
    return result == (int64_t)font_file.size &&
        vesa_load_psf2(font_data, font_file.size);
}

static bool cmdline_requests_installed_root(void)
{
    if (!executable_cmdline_request.response ||
        !executable_cmdline_request.response->cmdline)
        return false;
    const char *line = executable_cmdline_request.response->cmdline;
    const char token[] = "root=VeRsitileOS";
    for (uint64_t position = 0; position < 1024 && line[position];)
    {
        while (line[position] == ' ')
            position++;
        uint64_t start = position;
        while (position < 1024 && line[position] &&
               line[position] != ' ')
            position++;
        uint64_t length = position - start;
        if (length == sizeof(token) - 1)
        {
            uint64_t i = 0;
            while (i < length && line[start + i] == token[i])
                i++;
            if (i == length)
                return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------
   KERNEL MAIN
--------------------------------------------------------- */

void kmain(void)
{
    /* -----------------------------------------------------
       SERIAL INITIALIZATION
    ----------------------------------------------------- */

    if (!serial_init())
        halt_forever();

    serial_log(SERIAL_LOG_INFO, "================================");
    serial_log(SERIAL_LOG_INFO, "VeRsitileOS " KERNEL_VERSION " BOOT");
    serial_log(SERIAL_LOG_INFO, "================================");

    /* -----------------------------------------------------
       LIMINE VALIDATION
    ----------------------------------------------------- */

    if (limine_base_revision[2] != 0) {
        serial_log(
            SERIAL_LOG_ERROR,
            "Unsupported Limine base revision"
        );

        halt_forever();
    }

    if (!memmap_request.response ||
        !hhdm_request.response ||
        !rsdp_request.response ||
        !framebuffer_request.response) {

        serial_log(
            SERIAL_LOG_ERROR,
            "Missing Limine response"
        );

        halt_forever();
    }

    uint64_t hhdm_offset =
        hhdm_request.response->offset;

    uint64_t rsdp_physical =
        (uint64_t)(uintptr_t)rsdp_request.response->address;

    serial_write("HHDM offset: ");
    serial_write_hex(hhdm_offset);
    serial_putc('\n');

    serial_write("RSDP address: ");
    serial_write_hex(rsdp_physical);
    serial_putc('\n');

    print_memory_map();

    /* -----------------------------------------------------
       CPU TABLES
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing GDT");
    gdt_init();

    serial_log(SERIAL_LOG_INFO, "Initializing IDT");
    idt_init();

    serial_log(SERIAL_LOG_INFO, "Initializing ISR");
    isr_init();

    /* -----------------------------------------------------
       MEMORY MANAGER
    ----------------------------------------------------- */

    serial_log(
        SERIAL_LOG_INFO,
        "Initializing memory manager"
    );

    if (!mm_init(memmap_request.response, hhdm_offset)) {
        serial_log(
            SERIAL_LOG_ERROR,
            "Memory manager initialization FAILED"
        );

        halt_forever();
    }

    serial_log(
        SERIAL_LOG_INFO,
        "Paging, PMM, VMM, Region and Heap initialized"
    );

    print_pmm_statistics();

    /* -----------------------------------------------------
       APIC
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing APIC");

    if (!apic_init(rsdp_physical, hhdm_offset)) {
        serial_log(
            SERIAL_LOG_ERROR,
            "APIC initialization FAILED"
        );

        halt_forever();
    }

    pic_disable();

    /* -----------------------------------------------------
       IRQ
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing IRQ");
    irq_init();

    /* -----------------------------------------------------
       PIT TIMER
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing PIT");

    if (!timer_init()) {
        serial_log(
            SERIAL_LOG_ERROR,
            "PIT initialization FAILED"
        );

        halt_forever();
    }

    /* -----------------------------------------------------
       KEYBOARD
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing keyboard");
    keyboard_init();

    /* -----------------------------------------------------
       PS/2
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Initializing PS/2");

    if (!ps2_init()) {
        serial_log(
            SERIAL_LOG_WARN,
            "PS/2 initialization failed"
        );
    }

    serial_log(SERIAL_LOG_INFO, "Initializing PS/2 mouse");
    if (!mouse_init()) {
        serial_log(
            SERIAL_LOG_WARN,
            "PS/2 mouse initialization failed"
        );
    }

    /* Install the ring-3 syscall gate and mount the filesystem tree. */
    syscall_init();

    struct limine_module_response *module_response =
        module_request.response;
    bool initramfs_loaded = false;
    if (module_response && module_response->modules)
    {
        for (uint64_t i = 0; i < module_response->module_count; i++)
        {
            struct limine_file *module = module_response->modules[i];
            if (!module || !module->address || !module->size ||
                !module->string)
                continue;

            const char expected_name[] = "rootfs";
            uint64_t j = 0;
            while (expected_name[j] &&
                   module->string[j] == expected_name[j])
                j++;
            if (expected_name[j] || module->string[j])
                continue;

            initramfs_loaded =
                vfs_set_initramfs(module->address, module->size);
            break;
        }
    }

    if (initramfs_loaded)
    {
        serial_log(
            SERIAL_LOG_INFO,
            "Loaded root filesystem from Limine initramfs"
        );
    }
    else
    {
        serial_log(
            SERIAL_LOG_WARN,
            "Limine initramfs unavailable; trying IDE/ATAPI"
        );
    }

    if (!vfs_init()) {
        serial_log(
            SERIAL_LOG_ERROR,
            "Virtual filesystem initialization FAILED"
        );
        halt_forever();
    }

    if (!pci_init())
        serial_log(SERIAL_LOG_WARN, "PCI enumeration failed");
    if (pci_is_initialized())
    {
        if (!intel8265_probe())
            serial_log(SERIAL_LOG_WARN, "Intel 8265 Wi-Fi probe failed");
        else if (intel8265_device_count())
        {
            if (intel8265_prepare_device())
                (void)intel8265_load_firmware_image();
            else
                serial_log(SERIAL_LOG_WARN,
                           "Intel 8265 PCI/MMIO/DMA setup did not complete");
        }
        if (!ar5007eg_probe())
            serial_log(SERIAL_LOG_WARN, "AR5007EG Wi-Fi probe failed");
        else if (ar5007eg_device_count() &&
                 !ar5007eg_prepare_device())
            serial_log(SERIAL_LOG_WARN,
                       "AR5007EG PCI/MMIO setup did not complete");
    }
    hid_init();
    if (pci_is_initialized() && !xhci_probe())
        serial_log(SERIAL_LOG_WARN, "xHCI probe failed");
    if (pci_is_initialized())
    {
        (void)virtio_pci_device_count();
        (void)virtio_blk_init();
        (void)virtio_input_init();
    }

    uint32_t ata_devices = ata_pio_init();
    uint32_t partitions = mbr_scan();
    if (ata_devices)
    {
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write_dec(partitions);
        serial_write(" disk partitions discovered");
        serial_log_end();
    }
    if (vfs_mount_fat32_partitions())
        serial_log(SERIAL_LOG_INFO, "Mounted discovered FAT32 volumes");
    if (cmdline_requests_installed_root() &&
        !vfs_mount_installed_root())
        serial_log(
            SERIAL_LOG_ERROR,
            "Installed root requested but no matching ext2 volume found");
    if (vfs_mount_ext2_partitions())
        serial_log(SERIAL_LOG_INFO, "Mounted discovered ext2 volumes");

    /* -----------------------------------------------------
       FRAMEBUFFER
    ----------------------------------------------------- */

    bool uefi_boot = firmware_type_request.response &&
        firmware_type_request.response->firmware_type ==
            LIMINE_FIRMWARE_TYPE_EFI64;
    struct limine_framebuffer *framebuffer = NULL;
    if (framebuffer_request.response &&
        framebuffer_request.response->framebuffer_count &&
        framebuffer_request.response->framebuffers)
        framebuffer = framebuffer_request.response->framebuffers[0];

    serial_log(
        SERIAL_LOG_INFO,
        uefi_boot ? "Initializing UEFI GOP framebuffer"
                  : "Initializing boot framebuffer");

    bool framebuffer_ready = uefi_boot
        ? gop_init(framebuffer)
        : framebuffer && vesa_init(framebuffer);
    if (framebuffer_ready) {
        if (drm_init())
            serial_log(
                SERIAL_LOG_INFO,
                "Initialized framebuffer DRM core (vesa-drm0)");
        else
            serial_log(
                SERIAL_LOG_WARN,
                "Could not initialize framebuffer DRM core");
        if (load_system_font())
            serial_log(SERIAL_LOG_INFO, "Loaded Terminus PSF2 system font");
        else
            serial_log(SERIAL_LOG_WARN, "Using built-in fallback font");
        serial_console_reset_cursor();
        serial_console_replay();
    } else {
        serial_log(
            SERIAL_LOG_WARN,
            "Framebuffer unavailable or unsupported"
        );
    }

    /* -----------------------------------------------------
       SYSTEM TESTS
    ----------------------------------------------------- */

    serial_log(SERIAL_LOG_INFO, "Starting system tests");

    if (!tests_run()) {
        serial_log(
            SERIAL_LOG_ERROR,
            "VeRsitileOS system tests FAILED"
        );

        halt_forever();
    }

    serial_log(SERIAL_LOG_INFO, "Initializing isolated task manager");

    if (!tasking_init()) {
        serial_log(
            SERIAL_LOG_ERROR,
            "Task manager initialization FAILED"
        );

        halt_forever();
    }

    serial_log(
        SERIAL_LOG_INFO,
        "All system tests completed successfully"
    );

    /* -----------------------------------------------------
       FINISH INITIALIZATION
    ----------------------------------------------------- */

    serial_log(
        SERIAL_LOG_INFO,
        "VeRsitileOS initialization complete"
    );

    tasking_start();
}

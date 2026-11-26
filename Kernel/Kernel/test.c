#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "test.h"
#include "serial.h"
#include "pmm.h"
#include "paging.h"
#include "vmm.h"
#include "heap.h"
#include "drm.h"
#include "pci.h"
#include "xhci.h"
#include "hid.h"
#include "keyboard.h"
#include "mouse.h"
#include "intel8265.h"
#include "vfs.h"

#define PAGE_SIZE 4096

static volatile uint64_t vmm_kernel_data_probe;

/* ---------------------------------------------------------
   PMM TEST
--------------------------------------------------------- */

static bool test_pmm(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing PMM");

    uint64_t free_before = pmm_get_free_pages();
    uint64_t pages[3] = {0};

    for (uint64_t i = 0; i < 3; i++) {
        pages[i] = pmm_alloc_page();

        if (!pages[i]) {
            serial_log(SERIAL_LOG_ERROR, "PMM allocation FAILED");

            for (uint64_t j = 0; j < i; j++)
                pmm_free_page(pages[j]);

            return false;
        }

        for (uint64_t j = 0; j < i; j++) {
            if (pages[i] == pages[j]) {
                serial_log(SERIAL_LOG_ERROR, "PMM returned duplicate page");

                for (uint64_t k = 0; k <= i; k++)
                    pmm_free_page(pages[k]);

                return false;
            }
        }
    }

    for (uint64_t i = 0; i < 3; i++)
        pmm_free_page(pages[i]);

    if (pmm_get_free_pages() != free_before) {
        serial_log(SERIAL_LOG_ERROR, "PMM free-page count mismatch");
        return false;
    }

    free_before = pmm_get_free_pages();
    uint64_t contiguous = pmm_alloc_contiguous_pages(3);
    if (!contiguous)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "PMM contiguous allocation FAILED");
        return false;
    }
    for (uint64_t i = 0; i < 3; i++)
    {
        if (!pmm_is_allocated_page(contiguous + i * PAGE_SIZE))
        {
            for (uint64_t j = 0; j < 3; j++)
                pmm_free_page(contiguous + j * PAGE_SIZE);
            serial_log(
                SERIAL_LOG_ERROR,
                "PMM contiguous allocation tracking FAILED");
            return false;
        }
    }
    for (uint64_t i = 0; i < 3; i++)
        pmm_free_page(contiguous + i * PAGE_SIZE);
    if (pmm_get_free_pages() != free_before)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "PMM contiguous free-page count mismatch");
        return false;
    }

    serial_log(SERIAL_LOG_INFO, "PMM allocation and freeing PASSED");
    serial_log(SERIAL_LOG_INFO, "PMM contiguous allocation PASSED");
    return true;
}

/* ---------------------------------------------------------
   VMM TEST
--------------------------------------------------------- */

static bool test_vmm(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing VMM");

    const uint64_t virtual_address =
        0xFFFFC20000000000ULL;

    uint64_t physical_address = pmm_alloc_page();

    if (!physical_address) {
        serial_log(SERIAL_LOG_ERROR, "VMM test physical allocation FAILED");
        return false;
    }

    uint64_t flags = VMM_PRESENT | VMM_WRITABLE;

    if (paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    if (!vmm_map_page(
            virtual_address,
            physical_address,
            flags)) {

        serial_log(SERIAL_LOG_ERROR, "VMM mapping FAILED");
        pmm_free_page(physical_address);
        return false;
    }

    volatile uint64_t *memory =
        (volatile uint64_t *)(uintptr_t)virtual_address;

    memory[0] = 0x123456789ABCDEF0ULL;
    memory[1] = 0xCAFEBABEDEADBEEFULL;

    bool passed =
        memory[0] == 0x123456789ABCDEF0ULL &&
        memory[1] == 0xCAFEBABEDEADBEEFULL;

    if (!passed)
        serial_log(SERIAL_LOG_ERROR, "VMM read/write FAILED");

    uint64_t translated = vmm_translate(virtual_address);

    if (translated != physical_address) {
        serial_log(SERIAL_LOG_ERROR, "VMM translation FAILED");
        passed = false;
    }

    vmm_unmap_page(virtual_address);
    pmm_free_page(physical_address);

    if (!passed)
        return false;

    serial_log(SERIAL_LOG_INFO, "VMM mapping PASSED");
    serial_log(SERIAL_LOG_INFO, "VMM read/write PASSED");
    serial_log(SERIAL_LOG_INFO, "VMM translation PASSED");

    vmm_address_space_t isolated =
        vmm_create_address_space();
    uint64_t isolated_physical =
        pmm_alloc_page();
    const uint64_t user_address = 0x40000000ULL;

    if (!isolated || !isolated_physical)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "VMM address-space setup FAILED"
        );
        if (isolated_physical)
            pmm_free_page(isolated_physical);
        if (isolated)
            vmm_destroy_address_space(isolated);
        return false;
    }

    uint64_t user_flags =
        VMM_PRESENT | VMM_USER | VMM_WRITABLE;
    if (paging_nx_supported())
        user_flags |= VMM_NO_EXECUTE;

    uint64_t *isolated_page =
        paging_phys_to_virt(isolated_physical);
    if (!isolated_page)
    {
        pmm_free_page(isolated_physical);
        vmm_destroy_address_space(isolated);
        return false;
    }
    isolated_page[0] = 0x1020304050607080ULL;

    bool isolated_mapped = vmm_map_page_in(
        isolated,
        user_address,
        isolated_physical,
        user_flags
    );

    if (!isolated_mapped)
    {
        pmm_free_page(isolated_physical);
        vmm_destroy_address_space(isolated);
        return false;
    }

    if (!vmm_user_range_valid(
            isolated,
            user_address,
            sizeof(uint64_t),
            true) ||
        vmm_user_range_valid(
            vmm_current_address_space(),
            user_address,
            sizeof(uint64_t),
            true))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "VMM user mapping protection FAILED"
        );
        vmm_destroy_address_space(isolated);
        return false;
    }

    vmm_address_space_t cloned =
        vmm_clone_address_space(isolated);
    uint64_t cloned_physical =
        cloned ? vmm_translate_in(cloned, user_address) : 0;
    uint64_t cloned_kernel_data =
        cloned
            ? vmm_translate_in(
                cloned,
                (uint64_t)(uintptr_t)&vmm_kernel_data_probe
            )
            : 0;
    uint64_t *cloned_page =
        cloned_physical
            ? paging_phys_to_virt(cloned_physical)
            : NULL;

    if (!cloned || !cloned_physical || !cloned_kernel_data ||
        cloned_physical == isolated_physical ||
        !cloned_page ||
        cloned_page[0] != isolated_page[0])
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "VMM address-space clone FAILED"
        );
        if (cloned)
            vmm_destroy_address_space(cloned);
        vmm_destroy_address_space(isolated);
        return false;
    }

    if (!vmm_destroy_address_space(cloned) ||
        !vmm_destroy_address_space(isolated))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "VMM address-space cleanup FAILED"
        );
        return false;
    }

    serial_log(
        SERIAL_LOG_INFO,
        "VMM isolated address spaces PASSED"
    );

    return true;
}

/* ---------------------------------------------------------
   BASIC HEAP TESTS
--------------------------------------------------------- */

static bool test_heap_basic(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing basic heap allocation");

    uint8_t *a = kmalloc(64);

    if (!a) {
        serial_log(SERIAL_LOG_ERROR, "kmalloc FAILED");
        return false;
    }

    for (uint64_t i = 0; i < 64; i++)
        a[i] = (uint8_t)(i + 1);

    for (uint64_t i = 0; i < 64; i++) {
        if (a[i] != (uint8_t)(i + 1)) {
            serial_log(SERIAL_LOG_ERROR, "kmalloc read/write FAILED");
            kfree(a);
            return false;
        }
    }

    serial_log(SERIAL_LOG_INFO, "kmalloc PASSED");

    uint32_t *b = kcalloc(32, sizeof(uint32_t));

    if (!b) {
        serial_log(SERIAL_LOG_ERROR, "kcalloc FAILED");
        kfree(a);
        return false;
    }

    for (uint64_t i = 0; i < 32; i++) {
        if (b[i] != 0) {
            serial_log(SERIAL_LOG_ERROR, "kcalloc zeroing FAILED");
            kfree(a);
            kfree(b);
            return false;
        }
    }

    serial_log(SERIAL_LOG_INFO, "kcalloc zeroing PASSED");

    void *c = kmemalign(4096, 128);

    if (!c) {
        serial_log(SERIAL_LOG_ERROR, "kmemalign FAILED");
        kfree(a);
        kfree(b);
        return false;
    }

    if ((uintptr_t)c % 4096 != 0) {
        serial_log(SERIAL_LOG_ERROR, "kmemalign alignment FAILED");
        kfree(a);
        kfree(b);
        kfree(c);
        return false;
    }

    serial_log(SERIAL_LOG_INFO, "kmemalign PASSED");

    kfree(a);
    kfree(b);
    kfree(c);

    serial_log(SERIAL_LOG_INFO, "Basic heap tests PASSED");
    return true;
}

/* ---------------------------------------------------------
   KREALLOC TEST
--------------------------------------------------------- */

static bool test_krealloc(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing krealloc");

    uint8_t *buffer = kmalloc(32);

    if (!buffer) {
        serial_log(SERIAL_LOG_ERROR, "Initial allocation FAILED");
        return false;
    }

    for (uint64_t i = 0; i < 32; i++)
        buffer[i] = (uint8_t)(i + 1);

    uint8_t *grown = krealloc(buffer, 128);

    if (!grown) {
        serial_log(SERIAL_LOG_ERROR, "krealloc growth FAILED");
        kfree(buffer);
        return false;
    }

    for (uint64_t i = 0; i < 32; i++) {
        if (grown[i] != (uint8_t)(i + 1)) {
            serial_log(SERIAL_LOG_ERROR, "Growth data preservation FAILED");
            kfree(grown);
            return false;
        }
    }

    serial_log(SERIAL_LOG_INFO, "krealloc growth PASSED");
    serial_log(SERIAL_LOG_INFO, "Growth data preservation PASSED");

    for (uint64_t i = 32; i < 128; i++)
        grown[i] = 0xAA;

    uint8_t *shrunk = krealloc(grown, 16);

    if (!shrunk) {
        serial_log(SERIAL_LOG_ERROR, "krealloc shrinking FAILED");
        kfree(grown);
        return false;
    }

    for (uint64_t i = 0; i < 16; i++) {
        if (shrunk[i] != (uint8_t)(i + 1)) {
            serial_log(SERIAL_LOG_ERROR, "Shrink data preservation FAILED");
            kfree(shrunk);
            return false;
        }
    }

    kfree(shrunk);

    serial_log(SERIAL_LOG_INFO, "krealloc shrinking PASSED");
    serial_log(SERIAL_LOG_INFO, "krealloc tests PASSED");

    return true;
}

/* ---------------------------------------------------------
   KFREE TEST
--------------------------------------------------------- */

static bool test_kfree(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing kfree");

    void *a = kmalloc(128);

    if (!a) {
        serial_log(SERIAL_LOG_ERROR, "Initial allocation FAILED");
        return false;
    }

    kfree(a);

    void *b = kmalloc(128);

    if (!b) {
        serial_log(SERIAL_LOG_ERROR, "Allocation after free FAILED");
        return false;
    }

    kfree(b);

    serial_log(SERIAL_LOG_INFO, "kfree and reuse PASSED");
    return true;
}

/* ---------------------------------------------------------
   HEAP BLOCK SPLITTING TEST
--------------------------------------------------------- */

static bool test_heap_splitting(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing heap block splitting");

    void *large = kmalloc(1024);

    if (!large) {
        serial_log(SERIAL_LOG_ERROR, "Large allocation FAILED");
        return false;
    }

    kfree(large);

    void *a = kmalloc(128);

    if (!a) {
        serial_log(SERIAL_LOG_ERROR, "First small allocation FAILED");
        return false;
    }

    void *b = kmalloc(128);

    if (!b) {
        serial_log(SERIAL_LOG_ERROR, "Second small allocation FAILED");
        kfree(a);
        return false;
    }

    uintptr_t a_start = (uintptr_t)a;
    uintptr_t b_start = (uintptr_t)b;

    bool overlap =
        a_start < b_start + 128 &&
        b_start < a_start + 128;

    if (overlap || a == b) {
        serial_log(SERIAL_LOG_ERROR, "Heap allocation overlap detected");
        kfree(a);
        kfree(b);
        return false;
    }

    if (a != large) {
        serial_log(SERIAL_LOG_ERROR, "Original block was not reused");
        kfree(a);
        kfree(b);
        return false;
    }

    kfree(a);
    kfree(b);

    serial_log(SERIAL_LOG_INFO, "Heap block splitting PASSED");
    return true;
}

/* ---------------------------------------------------------
   HEAP BLOCK COALESCING TEST
--------------------------------------------------------- */

static bool test_heap_coalescing(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing heap block coalescing");

    void *a = kmalloc(256);
    void *b = kmalloc(256);
    void *c = kmalloc(256);

    if (!a || !b || !c) {
        serial_log(SERIAL_LOG_ERROR, "Coalescing setup FAILED");

        if (a) kfree(a);
        if (b) kfree(b);
        if (c) kfree(c);

        return false;
    }

    kfree(a);
    kfree(b);

    void *merged = kmalloc(400);

    if (!merged) {
        serial_log(SERIAL_LOG_ERROR, "Merged allocation FAILED");
        kfree(c);
        return false;
    }

    if (merged != a) {
        serial_log(SERIAL_LOG_ERROR, "Merged block reuse FAILED");
        kfree(merged);
        kfree(c);
        return false;
    }

    kfree(merged);
    kfree(c);

    serial_log(SERIAL_LOG_INFO, "Heap block coalescing PASSED");
    return true;
}

/* ---------------------------------------------------------
   HEAP GROWTH AND SHRINKING TEST
--------------------------------------------------------- */

static bool test_heap_growth(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing heap growth");

    uint64_t before = kheap_get_committed();

    void *large = kmalloc(3 * PAGE_SIZE);

    if (!large) {
        serial_log(SERIAL_LOG_ERROR, "Large heap allocation FAILED");
        return false;
    }

    uint64_t after_growth = kheap_get_committed();

    if (after_growth <= before) {
        serial_log(SERIAL_LOG_ERROR, "Heap did not grow");
        kfree(large);
        return false;
    }

    serial_log(SERIAL_LOG_INFO, "Heap growth PASSED");

    kfree(large);

    uint64_t after_free = kheap_get_committed();

    if (after_free >= after_growth) {
        serial_log(SERIAL_LOG_ERROR, "Heap shrinking FAILED");

        serial_write("  Before: ");
        serial_write_dec(before);
        serial_write(" bytes\n");

        serial_write("  After growth: ");
        serial_write_dec(after_growth);
        serial_write(" bytes\n");

        serial_write("  After free: ");
        serial_write_dec(after_free);
        serial_write(" bytes\n");

        return false;
    }

    serial_log(SERIAL_LOG_INFO, "Heap shrinking PASSED");

    return true;
}

static bool test_drm_framebuffers(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing DRM software framebuffers");
    if (!drm_is_initialized())
    {
        serial_log(
            SERIAL_LOG_INFO,
            "DRM framebuffer test skipped without a display");
        return true;
    }

    drm_device_info_t before;
    drm_device_info_t after;
    if (!drm_get_device_info(&before) ||
        !before.connected ||
        !before.supports_software_framebuffers ||
        before.mode.format != DRM_FORMAT_XRGB8888)
    {
        serial_log(SERIAL_LOG_ERROR, "DRM device info FAILED");
        return false;
    }
    if (drm_register_firmware_scanout(
            "invalid-mode",
            before.mode.width == UINT32_MAX
                ? before.mode.width - 1
                : before.mode.width + 1,
            before.mode.height))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM accepted a firmware scanout with mismatched dimensions");
        return false;
    }

    drm_framebuffer_t *framebuffer = NULL;
    if (!drm_framebuffer_create(
            16,
            16,
            DRM_FORMAT_XRGB8888,
            &framebuffer) ||
        !framebuffer ||
        !drm_framebuffer_clear(framebuffer, 0x00112233) ||
        !drm_framebuffer_set_pixel(framebuffer, 2, 3, 0x00AABBCC) ||
        !drm_framebuffer_fill_rect(
            framebuffer,
            8,
            8,
            32,
            32,
            0x00010203) ||
        framebuffer->pixels[3 * 16 + 2] != 0x00AABBCC ||
        framebuffer->pixels[15 * 16 + 15] != 0x00010203 ||
        !framebuffer->damage.valid ||
        framebuffer->damage.x != 0 ||
        framebuffer->damage.y != 0 ||
        framebuffer->damage.width != 16 ||
        framebuffer->damage.height != 16)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM framebuffer allocation or drawing FAILED");
        if (framebuffer)
            drm_framebuffer_destroy(framebuffer);
        return false;
    }

    if (!drm_get_device_info(&after) ||
        after.framebuffer_count != before.framebuffer_count + 1 ||
        drm_framebuffer_set_pixel(framebuffer, 16, 0, 0) ||
        drm_framebuffer_fill_rect(framebuffer, 16, 0, 1, 1, 0) ||
        drm_framebuffer_present(framebuffer))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM framebuffer bounds or mode validation FAILED");
        drm_framebuffer_destroy(framebuffer);
        return false;
    }

    drm_framebuffer_t *invalid_framebuffer = framebuffer;
    if (drm_framebuffer_create(
            16,
            16,
            (drm_pixel_format_t)0,
            &invalid_framebuffer))
    {
        drm_framebuffer_destroy(invalid_framebuffer);
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM accepted an unsupported pixel format");
        return false;
    }
    if (invalid_framebuffer)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM failed creation left a stale framebuffer result");
        drm_framebuffer_destroy(invalid_framebuffer);
        drm_framebuffer_destroy(framebuffer);
        return false;
    }

    drm_framebuffer_destroy(framebuffer);
    if (!drm_get_device_info(&after) ||
        after.framebuffer_count != before.framebuffer_count)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "DRM framebuffer cleanup FAILED");
        return false;
    }

    serial_log(
        SERIAL_LOG_INFO,
        "DRM software framebuffer and dirty tracking PASSED");
    return true;
}

static bool text_contains(
    const char *text,
    uint64_t length,
    const char *needle
)
{
    uint64_t needle_length = 0;
    while (needle[needle_length])
        needle_length++;
    if (!needle_length || needle_length > length)
        return false;
    for (uint64_t i = 0; i <= length - needle_length; i++)
    {
        uint64_t j = 0;
        while (j < needle_length && text[i + j] == needle[j])
            j++;
        if (j == needle_length)
            return true;
    }
    return false;
}

static bool test_pci_inventory(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing PCI inventory and device metadata");
    if (!pci_is_initialized())
    {
        serial_log(SERIAL_LOG_ERROR, "PCI inventory was not initialized");
        return false;
    }

    vfs_dirent_t pci_entries[2];
    vfs_dirent_t sysfs_entries[2];
    int64_t pci_count = vfs_list(
        "/dev/pci", 0, pci_entries, 2);
    int64_t sysfs_count = vfs_list(
        "/sys/bus/pci/devices", 0, sysfs_entries, 2);
    if (pci_count < 0 || sysfs_count != pci_count)
    {
        serial_log(SERIAL_LOG_ERROR, "PCI device directories FAILED");
        return false;
    }
    if (!pci_count)
    {
        serial_log(SERIAL_LOG_INFO, "PCI metadata test skipped without devices");
        return true;
    }

    const pci_device_t *first = pci_device_get(0);
    char bdf[PCI_BDF_NAME_LENGTH];
    char path[64] = "/dev/pci/";
    if (!first || !pci_format_bdf(first, bdf, sizeof(bdf)))
    {
        serial_log(SERIAL_LOG_ERROR, "PCI address formatting FAILED");
        return false;
    }
    uint64_t path_length = sizeof("/dev/pci/") - 1;
    for (uint64_t i = 0; bdf[i] && path_length < sizeof(path) - 1; i++)
        path[path_length++] = bdf[i];
    path[path_length] = '\0';

    vfs_file_t file;
    char metadata[512];
    if (vfs_open(path, &file) < 0 ||
        file.size > sizeof(metadata) ||
        vfs_read(&file, metadata, file.size) != (int64_t)file.size ||
        !text_contains(metadata, file.size, "Vendor: "))
    {
        serial_log(SERIAL_LOG_ERROR, "PCI metadata device read FAILED");
        return false;
    }

    char sysfs_path[96] = "/sys/bus/pci/devices/";
    uint64_t sysfs_path_length = sizeof("/sys/bus/pci/devices/") - 1;
    for (uint64_t i = 0; bdf[i]; i++)
        sysfs_path[sysfs_path_length++] = bdf[i];
    sysfs_path[sysfs_path_length++] = '/';
    const char *vendor_attribute = "vendor";
    for (uint64_t i = 0; vendor_attribute[i]; i++)
        sysfs_path[sysfs_path_length++] = vendor_attribute[i];
    sysfs_path[sysfs_path_length] = '\0';
    if (vfs_open(sysfs_path, &file) < 0 ||
        vfs_read(&file, metadata, file.size) != (int64_t)file.size ||
        file.size != 7 ||
        metadata[0] != '0' ||
        metadata[1] != 'x')
    {
        serial_log(SERIAL_LOG_ERROR, "PCI sysfs attributes FAILED");
        return false;
    }

    uint64_t attribute_start = sysfs_path_length - (sizeof("vendor") - 1);
    const char *resource_attribute = "resource";
    for (uint64_t i = 0; resource_attribute[i]; i++)
        sysfs_path[attribute_start + i] = resource_attribute[i];
    sysfs_path[attribute_start + sizeof("resource") - 1] = '\0';
    sysfs_path_length = attribute_start + sizeof("resource") - 1;
    if (vfs_open(sysfs_path, &file) < 0 ||
        !file.size ||
        file.size > sizeof(metadata) ||
        vfs_read(&file, metadata, file.size) != (int64_t)file.size ||
        (!text_contains(metadata, file.size, "BAR") &&
         !text_contains(metadata, file.size, "No BARs")))
    {
        serial_log(SERIAL_LOG_ERROR, "PCI resource attribute FAILED");
        return false;
    }

    attribute_start = sysfs_path_length - (sizeof("resource") - 1);
    const char *driver_attribute = "driver";
    for (uint64_t i = 0; driver_attribute[i]; i++)
        sysfs_path[attribute_start + i] = driver_attribute[i];
    sysfs_path[attribute_start + sizeof("driver") - 1] = '\0';
    sysfs_path_length = attribute_start + sizeof("driver") - 1;
    int64_t driver_open_result = vfs_open(sysfs_path, &file);
    int64_t driver_read_result = driver_open_result < 0
        ? driver_open_result
        : vfs_read(&file, metadata, file.size);
    if (driver_open_result < 0 ||
        file.size > sizeof(metadata) ||
        driver_read_result != (int64_t)file.size ||
        (!text_contains(metadata, file.size, "radeonhd") &&
         !text_contains(metadata, file.size, "unbound")))
    {
        serial_log(SERIAL_LOG_ERROR, "PCI driver binding attribute FAILED");
        return false;
    }

    attribute_start = sysfs_path_length - (sizeof("driver") - 1);
    const char *debug_attribute = "debug";
    for (uint64_t i = 0; debug_attribute[i]; i++)
        sysfs_path[attribute_start + i] = debug_attribute[i];
    sysfs_path[attribute_start + sizeof("debug") - 1] = '\0';
    if (vfs_open(sysfs_path, &file) < 0 ||
        file.size > sizeof(metadata) ||
        vfs_read(&file, metadata, file.size) != (int64_t)file.size ||
        !text_contains(metadata, file.size, "hardware_init: not attempted"))
    {
        serial_log(SERIAL_LOG_ERROR, "PCI driver debug sysfs FAILED");
        return false;
    }

    vfs_dirent_t category_entries[8];
    int64_t display_count = vfs_list(
        "/dev/dri", 0, category_entries, 8);
    int64_t sound_count = vfs_list(
        "/dev/snd", 0, category_entries, 8);
    uint32_t expected_display = pci_class_device_count(0x03, 0xFF);
    uint32_t expected_sound =
        pci_class_device_count(0x04, 0x01) +
        pci_class_device_count(0x04, 0x03);
    if (expected_display > 8)
        expected_display = 8;
    if (expected_sound > 8)
        expected_sound = 8;
    if (display_count < 0 || sound_count < 0 ||
        (uint64_t)display_count != expected_display ||
        (uint64_t)sound_count != expected_sound)
    {
        serial_log(SERIAL_LOG_ERROR, "PCI graphics/audio nodes FAILED");
        return false;
    }

    serial_log(SERIAL_LOG_INFO, "PCI inventory and metadata PASSED");
    return true;
}

static bool test_intel8265_match(void)
{
    pci_device_t supported = {
        .vendor_id = 0x8086,
        .device_id = 0x24FD,
        .class_code = 0x02
    };
    pci_device_t wrong_vendor = supported;
    wrong_vendor.vendor_id = 0x1234;
    pci_device_t wrong_device = supported;
    wrong_device.device_id = 0x100E;
    pci_device_t wrong_class = supported;
    wrong_class.class_code = 0x03;

    if (!intel8265_matches_device(&supported) ||
        intel8265_matches_device(NULL) ||
        intel8265_matches_device(&wrong_vendor) ||
        intel8265_matches_device(&wrong_device) ||
        intel8265_matches_device(&wrong_class))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "Intel 8265 PCI match test FAILED");
        return false;
    }

    for (uint32_t i = 0; i < intel8265_device_count(); i++)
    {
        const pci_device_t *device = intel8265_device_get(i);
        if (!intel8265_matches_device(device))
        {
            serial_log(
                SERIAL_LOG_ERROR,
                "Intel 8265 PCI inventory FAILED");
            return false;
        }
    }
    if (intel8265_device_get(intel8265_device_count()))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "Intel 8265 PCI inventory bounds FAILED");
        return false;
    }

    serial_log(
        SERIAL_LOG_INFO,
        "Intel 8265 / 8275 PCI detection PASSED");
    return true;
}

static void write_test_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static bool test_intel8265_firmware_parser(void)
{
    static const uint8_t crc_test_vector[] = "123456789";
    uint32_t checksum = 0;
    if (!intel8265_firmware_crc32(
            crc_test_vector, sizeof(crc_test_vector) - 1, &checksum) ||
        checksum != 0xCBF43926 ||
        intel8265_firmware_crc32(NULL, sizeof(crc_test_vector), &checksum) ||
        intel8265_firmware_crc32(crc_test_vector, 0, &checksum))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 CRC-32 known-vector check FAILED");
        return false;
    }

    uint8_t image[120] = {0};
    write_test_le32(image + 4, 0x0A4C5749U);
    write_test_le32(image + 72, INTEL8265_FIRMWARE_API_VERSION);
    write_test_le32(image + 76, 0x12345678);
    write_test_le32(image + 80, INTEL8265_TLV_SEC_RT);
    write_test_le32(image + 84, 12);
    write_test_le32(image + 88, 0x00404000);
    for (uint32_t i = 0; i < 8; i++)
        image[92 + i] = (uint8_t)(0xA0 + i);
    write_test_le32(image + 100, INTEL8265_TLV_SEC_INIT);
    write_test_le32(image + 104, 12);
    write_test_le32(image + 108, 0x00405000);
    for (uint32_t i = 0; i < 8; i++)
        image[112 + i] = (uint8_t)(0xB0 + i);

    intel8265_firmware_info_t info;
    intel8265_firmware_tlv_t tlv;
    if (!intel8265_firmware_inspect(image, sizeof(image), &info) ||
        info.api_version != INTEL8265_FIRMWARE_API_VERSION ||
        info.build != 0x12345678 || info.tlv_count != 2 ||
        info.runtime_section_count != 1 || info.init_section_count != 1 ||
        info.wowlan_section_count != 0 ||
        info.runtime_section_bytes != 8 || info.init_section_bytes != 8 ||
        info.wowlan_section_bytes != 0 ||
        info.image_crc32 != 0x7BF8ED6A ||
        !intel8265_firmware_crc32(
            image, sizeof(image), &checksum) ||
        checksum != info.image_crc32 ||
        !intel8265_firmware_tlv_get(image, sizeof(image), 0, &tlv) ||
        tlv.type != INTEL8265_TLV_SEC_RT || tlv.length != 12 ||
        tlv.data[11] != 0xA7 ||
        !intel8265_firmware_tlv_get(image, sizeof(image), 1, &tlv) ||
        tlv.type != INTEL8265_TLV_SEC_INIT || tlv.length != 12 ||
        tlv.data[11] != 0xB7 ||
        intel8265_firmware_tlv_get(image, sizeof(image), 2, &tlv))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware TLV parsing FAILED");
        return false;
    }

    uint8_t chunk[4] = {0};
    uint32_t chunk_type = 0;
    uint32_t chunk_size = 0;
    if (!intel8265_firmware_copy_tlv_chunk(
            image, sizeof(image), 0, 4,
            chunk, sizeof(chunk), &chunk_type, &chunk_size) ||
        chunk_type != INTEL8265_TLV_SEC_RT || chunk_size != sizeof(chunk) ||
        chunk[0] != 0xA0 || chunk[1] != 0xA1 ||
        chunk[2] != 0xA2 || chunk[3] != 0xA3 ||
        !intel8265_firmware_copy_tlv_chunk(
            image, sizeof(image), 0, 11,
            chunk, sizeof(chunk), &chunk_type, &chunk_size) ||
        chunk_size != 1 || chunk[0] != 0xA7 ||
        intel8265_firmware_copy_tlv_chunk(
            image, sizeof(image), 0, 12,
            chunk, sizeof(chunk), &chunk_type, &chunk_size))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware chunk staging checks FAILED");
        return false;
    }

    intel8265_firmware_section_t section;
    uint32_t device_address = 0;
    if (!intel8265_firmware_section_get(
            image, sizeof(image), 0, &section) ||
        section.type != INTEL8265_TLV_SEC_RT ||
        section.device_address != 0x00404000 ||
        section.size != 8 || section.data[0] != 0xA0 ||
        !intel8265_firmware_copy_section_chunk(
            image, sizeof(image), 0, 1,
            chunk, sizeof(chunk), &device_address, &chunk_size) ||
        device_address != 0x00404001 || chunk_size != 4 ||
        chunk[0] != 0xA1 || chunk[1] != 0xA2 ||
        chunk[2] != 0xA3 || chunk[3] != 0xA4 ||
        intel8265_firmware_copy_section_chunk(
            image, sizeof(image), 0, 8,
            chunk, sizeof(chunk), &device_address, &chunk_size))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware section decoding FAILED");
        return false;
    }

    if (!intel8265_firmware_image_section_get(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            0, &section) ||
        section.type != INTEL8265_TLV_SEC_RT ||
        !intel8265_firmware_image_section_get(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_INIT,
            0, &section) ||
        section.type != INTEL8265_TLV_SEC_INIT ||
        intel8265_firmware_image_section_get(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            1, &section))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 init/runtime section selection FAILED");
        return false;
    }

    uint64_t planned_bytes = 0;
    uint32_t planned_chunks = 0;
    if (!intel8265_firmware_image_plan(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            4, &planned_bytes, &planned_chunks) ||
        planned_bytes != 8 || planned_chunks != 2 ||
        !intel8265_firmware_image_plan(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_INIT,
            8, &planned_bytes, &planned_chunks) ||
        planned_bytes != 8 || planned_chunks != 1 ||
        intel8265_firmware_image_plan(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            3, &planned_bytes, &planned_chunks) ||
        intel8265_firmware_image_plan(
            image, sizeof(image),
            (intel8265_firmware_image_t)INTEL8265_TLV_SEC_WOWLAN,
            4, &planned_bytes, &planned_chunks))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware image planning FAILED");
        return false;
    }

    uint8_t image_chunk[8];
    if (!intel8265_firmware_copy_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            0, 0, image_chunk, sizeof(image_chunk),
            &device_address, &chunk_size) ||
        device_address != 0x00404000 || chunk_size != sizeof(image_chunk) ||
        image_chunk[0] != 0xA0 || image_chunk[7] != 0xA7 ||
        intel8265_firmware_copy_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            0, 2, image_chunk, sizeof(image_chunk),
            &device_address, &chunk_size))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 aligned image chunk checks FAILED");
        return false;
    }

    intel8265_firmware_cursor_t cursor = {0};
    bool complete = false;
    if (!intel8265_firmware_copy_next_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            &cursor, image_chunk, 4, &device_address, &chunk_size,
            &complete) ||
        device_address != 0x00404000 || chunk_size != 4 || complete ||
        cursor.section_index != 0 || cursor.offset != 4 ||
        image_chunk[0] != 0xA0 || image_chunk[3] != 0xA3 ||
        !intel8265_firmware_copy_next_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            &cursor, image_chunk, 4, &device_address, &chunk_size,
            &complete) ||
        device_address != 0x00404004 || chunk_size != 4 || !complete ||
        cursor.section_index != 1 || cursor.offset != 0 ||
        image_chunk[0] != 0xA4 || image_chunk[3] != 0xA7 ||
        !intel8265_firmware_copy_next_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            &cursor, image_chunk, 4, &device_address, &chunk_size,
            &complete) ||
        !complete || chunk_size != 0 ||
        intel8265_firmware_copy_next_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            &(intel8265_firmware_cursor_t){.section_index = 2},
            image_chunk, sizeof(image_chunk), &device_address, &chunk_size,
            &complete) ||
        intel8265_firmware_copy_next_image_chunk(
            image, sizeof(image), INTEL8265_FIRMWARE_IMAGE_RUNTIME,
            &(intel8265_firmware_cursor_t){.section_index = 1, .offset = 4},
            image_chunk, sizeof(image_chunk), &device_address, &chunk_size,
            &complete))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 sequential firmware chunk checks FAILED");
        return false;
    }

    uint8_t bad_magic[sizeof(image)];
    for (uint64_t i = 0; i < sizeof(image); i++)
        bad_magic[i] = image[i];
    bad_magic[4] ^= 0x01;
    if (intel8265_firmware_inspect(
            bad_magic, sizeof(bad_magic), &info))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware magic validation FAILED");
        return false;
    }

    uint8_t bad_api[sizeof(image)];
    for (uint64_t i = 0; i < sizeof(image); i++)
        bad_api[i] = image[i];
    write_test_le32(bad_api + 72, INTEL8265_FIRMWARE_API_VERSION + 1);
    if (intel8265_firmware_inspect(bad_api, sizeof(bad_api), &info))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware API validation FAILED");
        return false;
    }

    uint8_t truncated[89] = {0};
    write_test_le32(truncated + 4, 0x0A4C5749U);
    write_test_le32(
        truncated + 72, INTEL8265_FIRMWARE_API_VERSION);
    write_test_le32(truncated + 80, INTEL8265_TLV_SEC_RT);
    write_test_le32(truncated + 84, 12);
    if (intel8265_firmware_inspect(
            truncated, sizeof(truncated), &info))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 truncated firmware validation FAILED");
        return false;
    }

    uint8_t unaligned_destination[sizeof(image)];
    for (uint64_t i = 0; i < sizeof(image); i++)
        unaligned_destination[i] = image[i];
    write_test_le32(unaligned_destination + 88, 0xAAAABBBB);
    if (!intel8265_firmware_inspect(
            unaligned_destination, sizeof(unaligned_destination), &info) ||
    info.runtime_section_bytes != 8 || info.init_section_bytes != 8 ||
    !intel8265_firmware_section_get(
            unaligned_destination, sizeof(unaligned_destination), 0,
            &section) ||
        section.device_address != 0xAAAABBBB)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 special firmware destination parsing FAILED");
        return false;
    }

    uint8_t misaligned_record[sizeof(image)];
    for (uint64_t i = 0; i < sizeof(image); i++)
        misaligned_record[i] = image[i];
    write_test_le32(misaligned_record + 84, 14);
    if (intel8265_firmware_inspect(
            misaligned_record, sizeof(misaligned_record), &info))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 TLV alignment check FAILED");
        return false;
    }

    uint8_t empty_section[92] = {0};
    write_test_le32(empty_section + 4, 0x0A4C5749U);
    write_test_le32(
        empty_section + 72, INTEL8265_FIRMWARE_API_VERSION);
    write_test_le32(empty_section + 80, INTEL8265_TLV_SEC_RT);
    write_test_le32(empty_section + 84, sizeof(uint32_t));
    if (intel8265_firmware_inspect(
            empty_section, sizeof(empty_section), &info))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 empty firmware section check FAILED");
        return false;
    }

    serial_log(SERIAL_LOG_INFO,
               "Intel 8265 firmware container validation PASSED");
    return true;
}

static bool test_xhci_match(void)
{
    pci_device_t supported = {
        .class_code = 0x0C,
        .subclass = 0x03,
        .programming_interface = 0x30
    };
    pci_device_t uhci = supported;
    uhci.programming_interface = 0x00;
    pci_device_t ehci = supported;
    ehci.programming_interface = 0x20;
    pci_device_t non_usb = supported;
    non_usb.class_code = 0x02;

    if (!xhci_matches_device(&supported) ||
        xhci_matches_device(NULL) ||
        xhci_matches_device(&uhci) ||
        xhci_matches_device(&ehci) ||
        xhci_matches_device(&non_usb) ||
        xhci_controller_get(xhci_controller_count()))
    {
        serial_log(SERIAL_LOG_ERROR, "xHCI PCI match test FAILED");
        return false;
    }

    serial_log(SERIAL_LOG_INFO, "xHCI PCI match test PASSED");
    return true;
}

static bool test_hid_boot_reports(void)
{
    keyboard_init();
    hid_init();

    const uint8_t key_down[8] = {
        0, 0, 0x04, 0, 0, 0, 0, 0
    };
    const uint8_t key_up[8] = {0};
    keyboard_event_t key_events[2];
    if (!hid_handle_boot_keyboard_report(key_down) ||
        keyboard_read_events(key_events, sizeof(key_events[0])) !=
            (int64_t)sizeof(key_events[0]) ||
        key_events[0].character != 'a' ||
        !key_events[0].pressed ||
        !hid_handle_boot_keyboard_report(key_up) ||
        keyboard_read_events(key_events, sizeof(key_events[0])) !=
            (int64_t)sizeof(key_events[0]) ||
        key_events[0].pressed ||
        hid_handle_boot_keyboard_report(
            (const uint8_t[8]){0, 0, 1, 0, 0, 0, 0, 0}))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "USB HID boot keyboard report test FAILED");
        return false;
    }

    const uint8_t mouse_report[3] = {0x05, 7, 2};
    mouse_event_t mouse_event;
    if (!hid_handle_boot_mouse_report(mouse_report) ||
        mouse_read_events(&mouse_event, sizeof(mouse_event)) !=
            (int64_t)sizeof(mouse_event) ||
        mouse_event.dx != 7 || mouse_event.dy != 2 ||
        mouse_event.buttons != 0x05)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "USB HID boot mouse report test FAILED");
        return false;
    }

    serial_log(
        SERIAL_LOG_INFO,
        "USB HID boot keyboard and mouse reports PASSED");
    keyboard_init();
    return true;
}

static bool test_tmp_directory(void)
{
    serial_log(SERIAL_LOG_INFO, "Testing temporary directory");

    vfs_dirent_t entries[4];
    if (!vfs_is_directory("/tmp") ||
        vfs_list("/tmp", 0, entries, 4) != 0 ||
        vfs_mkdir("/tmp/test") < 0)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "Temporary directory setup FAILED");
        return false;
    }

    int64_t count = vfs_list("/tmp", 0, entries, 4);
    bool found_test_directory = count == 1;
    static const char expected_name[] = "test";
    if (found_test_directory)
    {
        for (size_t i = 0; i < sizeof(expected_name); i++)
        {
            if (entries[0].name[i] != expected_name[i])
            {
                found_test_directory = false;
                break;
            }
        }
    }

    bool removed = vfs_rmdir("/tmp/test") == 0;
    bool root_preserved = vfs_rmdir("/tmp") < 0;
    int64_t remaining_count = vfs_list("/tmp", 0, entries, 4);
    if (!found_test_directory)
        serial_log(SERIAL_LOG_ERROR, "Temporary directory entry missing");
    if (!removed)
        serial_log(SERIAL_LOG_ERROR, "Temporary directory removal failed");
    if (!root_preserved)
        serial_log(SERIAL_LOG_ERROR, "Temporary directory root was removed");
    if (remaining_count != 0)
        serial_log(SERIAL_LOG_ERROR, "Temporary directory is not empty");
    if (!found_test_directory || !removed || !root_preserved ||
        remaining_count != 0)
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "Temporary directory behavior FAILED");
        return false;
    }

    serial_log(
        SERIAL_LOG_INFO,
        "Temporary directory isolation PASSED");
    return true;
}

/* ---------------------------------------------------------
   TEST RUNNER
--------------------------------------------------------- */

typedef bool (*test_function_t)(void);

struct test_entry {
    const char *name;
    test_function_t function;
};

bool tests_run(void)
{
    static const struct test_entry tests[] = {
        { "Physical Memory Manager", test_pmm },
        { "Virtual Memory Manager",  test_vmm },
        { "Basic Heap",              test_heap_basic },
        { "Heap Reallocation",       test_krealloc },
        { "Heap Free",               test_kfree },
        { "Heap Splitting",          test_heap_splitting },
        { "Heap Coalescing",         test_heap_coalescing },
        { "Heap Growth/Shrinking",   test_heap_growth },
        { "DRM Software Framebuffers", test_drm_framebuffers },
        { "PCI Inventory", test_pci_inventory },
        { "xHCI Controller Match", test_xhci_match },
        { "USB HID Boot Reports", test_hid_boot_reports },
        { "Intel 8265 / 8275 Detection", test_intel8265_match },
        { "Intel 8265 Firmware Parser", test_intel8265_firmware_parser },
        { "Temporary Directory", test_tmp_directory }
    };

    const uint64_t total =
        sizeof(tests) / sizeof(tests[0]);

    serial_log(SERIAL_LOG_INFO, "================================");
    serial_log(SERIAL_LOG_INFO, "     VeRsitileOS SYSTEM TESTS");
    serial_log(SERIAL_LOG_INFO, "================================");

    for (uint64_t i = 0; i < total; i++) {
        serial_write("[TEST] ");
        serial_write(tests[i].name);
        serial_putc('\n');

        if (!tests[i].function()) {
            serial_log(SERIAL_LOG_ERROR, "SYSTEM TEST FAILED");
            serial_write("[TEST] Failed at: ");
            serial_write(tests[i].name);
            serial_putc('\n');
            return false;
        }

        serial_log(SERIAL_LOG_INFO, "TEST PASSED");
    }

    serial_log(SERIAL_LOG_INFO, "================================");
    serial_log(SERIAL_LOG_INFO, "ALL SYSTEM TESTS PASSED");
    serial_log(SERIAL_LOG_INFO, "================================");

    return true;
}
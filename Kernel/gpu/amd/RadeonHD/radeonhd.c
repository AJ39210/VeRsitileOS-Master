#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "amd_drm.h"
#include "pci.h"
#include "radeonhd.h"
#include "serial.h"

typedef struct
{
    uint16_t device_id;
    radeonhd_generation_t generation;
} radeonhd_id_t;

static const radeonhd_id_t supported_devices[] = {
    { 0x6898, RADEONHD_GENERATION_EVERGREEN },
    { 0x6899, RADEONHD_GENERATION_EVERGREEN },
    { 0x689C, RADEONHD_GENERATION_EVERGREEN },
    { 0x689D, RADEONHD_GENERATION_EVERGREEN },
    { 0x68B8, RADEONHD_GENERATION_EVERGREEN },
    { 0x68B9, RADEONHD_GENERATION_EVERGREEN },
    { 0x68BA, RADEONHD_GENERATION_EVERGREEN },
    { 0x68D8, RADEONHD_GENERATION_EVERGREEN },
    { 0x68D9, RADEONHD_GENERATION_EVERGREEN },
    { 0x68DA, RADEONHD_GENERATION_EVERGREEN },
    { 0x68F9, RADEONHD_GENERATION_EVERGREEN },
    { 0x68FA, RADEONHD_GENERATION_EVERGREEN },
    { 0x6718, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6719, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x671D, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x671F, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6738, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6739, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6740, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6741, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6742, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6778, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x6779, RADEONHD_GENERATION_NORTHERN_ISLANDS },
    { 0x677B, RADEONHD_GENERATION_NORTHERN_ISLANDS }
};

static radeonhd_device_t devices[RADEONHD_DEVICE_LIMIT];
static uint32_t device_count;
static bool initialized;
static radeonhd_probe_stats_t probe_stats;

static void probe_firmware_scanout(radeonhd_device_t *device)
{
    amd_drm_scanout_info_t info;
    if (!device || !device->pci_device ||
        !amd_drm_probe_firmware_scanout(device->pci_device, &info))
        return;
    device->mmio_mapped = info.mmio_mapped;
    device->scanout_active = info.scanout_active;
    device->drm_attached = info.drm_attached;
    device->scanout_width = info.scanout_width;
    device->scanout_height = info.scanout_height;
}

radeonhd_generation_t radeonhd_classify_device(
    uint16_t vendor_id,
    uint16_t device_id
)
{
    if (vendor_id != 0x1002)
        return RADEONHD_GENERATION_NONE;

    for (uint32_t i = 0;
         i < sizeof(supported_devices) / sizeof(supported_devices[0]);
         i++)
    {
        if (supported_devices[i].device_id == device_id)
            return supported_devices[i].generation;
    }
    return RADEONHD_GENERATION_NONE;
}

const char *radeonhd_generation_name(radeonhd_generation_t generation)
{
    switch (generation)
    {
        case RADEONHD_GENERATION_EVERGREEN:
            return "Evergreen";
        case RADEONHD_GENERATION_NORTHERN_ISLANDS:
            return "Northern Islands";
        default:
            return "unsupported";
    }
}

bool radeonhd_init(void)
{
    device_count = 0;
    probe_stats = (radeonhd_probe_stats_t){0};
    initialized = false;
    if (!pci_is_initialized())
        return false;

    probe_stats.pci_functions_seen = pci_device_count();
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *pci_device = pci_device_get(i);
        if (!pci_device)
            continue;
        if (pci_device->class_code != 0x03)
            continue;

        probe_stats.display_controllers_seen++;
        if (pci_device->vendor_id != 0x1002)
            continue;
        probe_stats.amd_display_controllers_seen++;

        radeonhd_generation_t generation = radeonhd_classify_device(
            pci_device->vendor_id, pci_device->device_id);
        if (generation == RADEONHD_GENERATION_NONE)
            continue;

        if (device_count == RADEONHD_DEVICE_LIMIT)
        {
            serial_log(
                SERIAL_LOG_WARN,
                "Radeon HD probe reached its device limit");
            break;
        }

        devices[device_count++] = (radeonhd_device_t) {
            .pci_device = pci_device,
            .generation = generation,
            .generation_name = radeonhd_generation_name(generation)
        };
        probe_stats.supported_devices++;
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("Radeon HD ");
        serial_write(radeonhd_generation_name(generation));
        serial_write(" device found: ");
        serial_write(pci_device_name(pci_device));
        serial_log_end();
    }

    for (uint32_t i = 0; i < device_count; i++)
    {
        radeonhd_device_t *device = &devices[i];
        probe_firmware_scanout(device);
        if (device->drm_attached)
        {
            serial_log(
                SERIAL_LOG_INFO,
                "Radeon HD firmware scanout attached to software DRM");
            break;
        }
    }

    initialized = true;
    if (!device_count)
        serial_log(
            SERIAL_LOG_INFO,
            "Radeon HD probe found no supported Evergreen/Northern Islands GPU");
    else
        serial_log(
            SERIAL_LOG_INFO,
            "Radeon HD display probe initialized (firmware mode retained)");
    return true;
}

bool radeonhd_is_initialized(void)
{
    return initialized;
}

uint32_t radeonhd_device_count(void)
{
    return initialized ? device_count : 0;
}

bool radeonhd_get_probe_stats(radeonhd_probe_stats_t *stats)
{
    if (!initialized || !stats)
        return false;
    *stats = probe_stats;
    return true;
}

const radeonhd_device_t *radeonhd_device_get(uint32_t index)
{
    if (!initialized || index >= device_count)
        return 0;
    return &devices[index];
}

const radeonhd_device_t *radeonhd_find_device(
    const pci_device_t *pci_device
)
{
    if (!initialized || !pci_device)
        return 0;
    for (uint32_t i = 0; i < device_count; i++)
    {
        if (devices[i].pci_device == pci_device)
            return &devices[i];
    }
    return 0;
}

static bool string_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right)
    {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static size_t append_text(
    char *buffer,
    size_t capacity,
    size_t position,
    const char *text
)
{
    while (*text)
    {
        if (position == capacity)
            return 0;
        buffer[position++] = *text++;
    }
    return position;
}

static size_t append_hex(
    char *buffer,
    size_t capacity,
    size_t position,
    uint32_t value,
    uint8_t digits
)
{
    static const char hex[] = "0123456789abcdef";
    if (position > capacity || digits > capacity - position)
        return 0;
    for (uint8_t i = 0; i < digits; i++)
    {
        uint8_t shift = (uint8_t)((digits - i - 1) * 4);
        buffer[position++] = hex[(value >> shift) & 0xF];
    }
    return position;
}

static size_t append_decimal(
    char *buffer,
    size_t capacity,
    size_t position,
    uint32_t value
)
{
    char digits[10];
    size_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    if (position > capacity || count > capacity - position)
        return 0;
    while (count)
        buffer[position++] = digits[--count];
    return position;
}

size_t radeonhd_format_debug(
    const pci_device_t *pci_device,
    char *buffer,
    size_t capacity
)
{
    if (!pci_device || !buffer || !capacity)
        return 0;

    size_t position = 0;
    const radeonhd_device_t *device = radeonhd_find_device(pci_device);
    radeonhd_generation_t generation = radeonhd_classify_device(
        pci_device->vendor_id, pci_device->device_id);
    char bdf[PCI_BDF_NAME_LENGTH];
    if (!pci_format_bdf(pci_device, bdf, sizeof(bdf)))
        return 0;

    position = append_text(buffer, capacity, position, "driver: ");
    if (!position)
        return 0;
    position = append_text(
        buffer, capacity, position, device ? "radeonhd\n" : "unbound\n");
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "pci: ");
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, bdf);
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "\nmodel: ");
    if (!position)
        return 0;
    position = append_text(
        buffer,
        capacity,
        position,
        pci_device_name(pci_device));
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "\ngeneration: ");
    if (!position)
        return 0;
    position = append_text(
        buffer,
        capacity,
        position,
        radeonhd_generation_name(generation));
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "\nrevision: 0x");
    if (!position)
        return 0;
    position = append_hex(
        buffer, capacity, position, pci_device->revision, 2);
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "\nbar_count: ");
    if (!position)
        return 0;
    position = append_decimal(
        buffer, capacity, position, pci_device->bar_count);
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "\nresources:\n");
    if (!position)
        return 0;
    size_t resources_length = pci_format_resources(
        pci_device, buffer + position, capacity - position);
    if (!resources_length)
        return 0;
    position += resources_length;
    position = append_text(buffer, capacity, position, "\nmmio: ");
    if (!position)
        return 0;
    position = append_text(
        buffer, capacity, position, device && device->mmio_mapped
            ? "mapped\n" : "not mapped\n");
    if (!position)
        return 0;
    position = append_text(buffer, capacity, position, "scanout: ");
    if (!position)
        return 0;
    position = append_text(
        buffer, capacity, position, device && device->scanout_active
            ? "firmware CRTC0 active\n" : "not detected\n");
    if (!position)
        return 0;
    if (device && device->scanout_active)
    {
        position = append_text(
            buffer, capacity, position, "mode: ");
        if (!position)
            return 0;
        position = append_decimal(
            buffer, capacity, position, device->scanout_width);
        if (!position)
            return 0;
        position = append_text(buffer, capacity, position, "x");
        if (!position)
            return 0;
        position = append_decimal(
            buffer, capacity, position, device->scanout_height);
        if (!position)
            return 0;
        position = append_text(buffer, capacity, position, "\n");
        if (!position)
            return 0;
    }
    position = append_text(
        buffer,
        capacity,
        position,
        device && device->drm_attached
            ? "drm: software framebuffers on firmware scanout; "
              "no modesetting or acceleration\n"
            : "drm: unavailable; firmware mode is retained\n");
    return position;
}

size_t radeonhd_format_attribute(
    const pci_device_t *pci_device,
    const char *attribute,
    char *buffer,
    size_t capacity
)
{
    if (!pci_device || !attribute || !buffer || !capacity)
        return 0;

    const radeonhd_device_t *device = radeonhd_find_device(pci_device);
    const char *value;
    if (string_equal(attribute, "driver"))
        value = device ? "radeonhd\n" : "unbound\n";
    else if (string_equal(attribute, "generation"))
        value = device ? device->generation_name : "unknown\n";
    else if (string_equal(attribute, "status"))
        value = device
            ? device->drm_attached
                ? "firmware scanout attached to software DRM; no modesetting\n"
                : device->scanout_active
                    ? "active firmware mode does not match boot framebuffer\n"
                : device->mmio_mapped
                    ? "MMIO mapped; no active firmware scanout\n"
                    : "PCI device identified; firmware scanout unavailable\n"
            : "unbound\n";
    else if (string_equal(attribute, "debug"))
        return radeonhd_format_debug(pci_device, buffer, capacity);
    else
        return 0;

    return append_text(buffer, capacity, 0, value);
}

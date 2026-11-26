#include <stdbool.h>
#include <stdint.h>

#include "intel_drm.h"
#include "intel_hd2000.h"
#include "pci.h"
#include "serial.h"

#define INTEL_VENDOR_ID 0x8086
#define INTEL_HD_GRAPHICS_2000_DEVICE_ID 0x0102

static const pci_device_t *devices[INTEL_HD2000_DEVICE_LIMIT];
static uint32_t device_count;

bool intel_hd2000_matches_device(const pci_device_t *device)
{
    return device &&
        device->vendor_id == INTEL_VENDOR_ID &&
        device->device_id == INTEL_HD_GRAPHICS_2000_DEVICE_ID &&
        device->class_code == 0x03;
}

bool intel_hd2000_probe(void)
{
    device_count = 0;
    if (!pci_is_initialized())
        return false;

    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!intel_hd2000_matches_device(device))
            continue;
        if (device_count == INTEL_HD2000_DEVICE_LIMIT)
            break;
        devices[device_count++] = device;
    }

    if (!device_count)
    {
        serial_log(
            SERIAL_LOG_INFO,
            "Intel HD Graphics 2000 probe found no supported GPU");
        return true;
    }

    uint32_t attached_count = 0;
    for (uint32_t i = 0; i < device_count; i++)
    {
        intel_drm_scanout_info_t scanout;
        if (!intel_drm_probe_firmware_scanout(devices[i], &scanout))
        {
            serial_log(
                SERIAL_LOG_WARN,
                "Intel HD Graphics 2000 MMIO scanout probe failed");
            continue;
        }
        if (scanout.drm_attached)
        {
            attached_count++;
            serial_log(
                SERIAL_LOG_INFO,
                "Intel HD Graphics 2000 firmware scanout attached to software DRM");
        }
        else if (scanout.scanout_active)
        {
            serial_log(
                SERIAL_LOG_INFO,
                "Intel HD Graphics 2000 active pipe mode does not match boot framebuffer");
        }
        else
        {
            serial_log(
                SERIAL_LOG_INFO,
                "Intel HD Graphics 2000 has no active display pipe");
        }
    }

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("Intel HD Graphics 2000 detected (");
    serial_write_dec(device_count);
    serial_write(" device(s); ");
    serial_write_dec(attached_count);
    serial_write(" firmware scanout(s) attached; no modesetting or acceleration");
    serial_log_end();
    return true;
}

uint32_t intel_hd2000_device_count(void)
{
    return device_count;
}

const pci_device_t *intel_hd2000_device_get(uint32_t index)
{
    return index < device_count ? devices[index] : NULL;
}

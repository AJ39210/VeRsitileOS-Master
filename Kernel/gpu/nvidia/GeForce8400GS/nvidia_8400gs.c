#include <stdbool.h>
#include <stdint.h>

#include "nvidia_drm.h"
#include "nvidia_8400gs.h"
#include "pci.h"
#include "serial.h"

#define NVIDIA_VENDOR_ID 0x10DE
#define NVIDIA_GEFORCE_8400GS_DEVICE_ID 0x0422

static const pci_device_t *devices[NVIDIA_8400GS_DEVICE_LIMIT];
static uint32_t device_count;

bool nvidia_8400gs_matches_device(const pci_device_t *device)
{
    return device &&
        device->vendor_id == NVIDIA_VENDOR_ID &&
        device->device_id == NVIDIA_GEFORCE_8400GS_DEVICE_ID &&
        device->class_code == 0x03;
}

bool nvidia_8400gs_probe(void)
{
    device_count = 0;
    if (!pci_is_initialized())
        return false;

    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!nvidia_8400gs_matches_device(device))
            continue;
        if (device_count == NVIDIA_8400GS_DEVICE_LIMIT)
            break;
        devices[device_count++] = device;
    }

    if (!device_count)
    {
        serial_log(
            SERIAL_LOG_INFO,
            "NVIDIA GeForce 8400 GS probe found no supported GPU");
        return true;
    }

    uint32_t attached_count = 0;
    for (uint32_t i = 0; i < device_count; i++)
    {
        nvidia_drm_scanout_info_t scanout;
        if (!nvidia_drm_probe_firmware_scanout(devices[i], &scanout))
        {
            serial_log(
                SERIAL_LOG_WARN,
                "NVIDIA GeForce 8400 GS MMIO scanout probe failed");
            continue;
        }
        if (scanout.drm_attached)
        {
            attached_count++;
            serial_log(
                SERIAL_LOG_INFO,
                "NVIDIA GeForce 8400 GS firmware scanout attached to software DRM");
        }
        else if (scanout.graphics_scanout_selected)
        {
            serial_log(
                SERIAL_LOG_INFO,
                "NVIDIA GeForce 8400 GS scanout selected; boot framebuffer mode retained");
        }
        else
        {
            serial_log(
                SERIAL_LOG_INFO,
                "NVIDIA GeForce 8400 GS is in VGA-compatible scanout mode");
        }
    }

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("NVIDIA GeForce 8400 GS detected (");
    serial_write_dec(device_count);
    serial_write(" device(s); ");
    serial_write_dec(attached_count);
    serial_write(" firmware scanout(s) attached; no modesetting or acceleration");
    serial_log_end();
    return true;
}

uint32_t nvidia_8400gs_device_count(void)
{
    return device_count;
}

const pci_device_t *nvidia_8400gs_device_get(uint32_t index)
{
    return index < device_count ? devices[index] : NULL;
}

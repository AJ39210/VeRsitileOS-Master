#include <stdbool.h>
#include <stdint.h>

#include "ar5007eg.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"

#define AR5007EG_MMIO_MIN_SIZE 0x1000U
#define AR5007EG_MMIO_MAX_SIZE 0x100000U

static const pci_device_t *devices[PCI_DEVICE_LIMIT];
static uint32_t device_count;
static ar5007eg_resources_t resources;

bool ar5007eg_probe(void)
{
    if (!pci_is_initialized())
        return false;

    device_count = 0;
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!ar5007eg_matches_device(device))
            continue;
        if (device_count == PCI_DEVICE_LIMIT)
            break;
        devices[device_count++] = device;
    }

    if (device_count)
    {
        serial_log_begin(SERIAL_LOG_INFO);
        serial_write("Detected Qualcomm Atheros AR5007EG/AR2425 (");
        serial_write_dec(device_count);
        serial_write(" device(s)); wireless operation is not enabled yet");
        serial_log_end();
    }
    return true;
}

bool ar5007eg_prepare_device(void)
{
    if (!device_count)
        return false;
    if (resources.mmio)
        return true;

    const pci_device_t *device = devices[0];
    pci_bar_info_t bar;
    if (!pci_get_bar_info(device, 0, &bar) || bar.is_io ||
        !bar.base || bar.size < AR5007EG_MMIO_MIN_SIZE ||
        bar.size > AR5007EG_MMIO_MAX_SIZE)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "AR5007EG BAR0 is not a valid bounded MMIO region");
        return false;
    }

    if (!pci_enable_device(device, true, false))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Could not enable AR5007EG PCI memory decoding");
        return false;
    }

    volatile uint8_t *mmio = ioremap(bar.base, bar.size);
    if (!mmio)
    {
        serial_log(SERIAL_LOG_ERROR, "Could not map AR5007EG BAR0");
        return false;
    }

    resources = (ar5007eg_resources_t){
        .mmio_physical = bar.base,
        .mmio_size = bar.size,
        .mmio = mmio
    };
    serial_log(SERIAL_LOG_INFO,
               "AR5007EG BAR0 mapped; radio remains stopped");
    return true;
}

bool ar5007eg_resources_get(ar5007eg_resources_t *result)
{
    if (!result || !resources.mmio)
        return false;
    *result = resources;
    return true;
}

uint32_t ar5007eg_device_count(void)
{
    return device_count;
}

const pci_device_t *ar5007eg_device_get(uint32_t index)
{
    if (index >= device_count)
        return NULL;
    return devices[index];
}

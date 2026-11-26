#include <stddef.h>
#include <stdint.h>

#include "block.h"

static block_device_t devices[BLOCK_DEVICE_LIMIT];
static uint32_t device_count;

bool block_register(const block_device_t *device)
{
    if (!device || !device->name[0] || !device->sector_count ||
        device->sector_size != BLOCK_SECTOR_SIZE || !device->read ||
        (device->writable && !device->write) ||
        device_count >= BLOCK_DEVICE_LIMIT)
        return false;

    size_t i = 0;
    while (i < sizeof(device->name) && device->name[i])
    {
        devices[device_count].name[i] = device->name[i];
        i++;
    }
    if (i == sizeof(device->name))
        return false;
    devices[device_count].name[i] = '\0';
    devices[device_count].sector_count = device->sector_count;
    devices[device_count].sector_size = device->sector_size;
    devices[device_count].writable = device->writable;
    devices[device_count].context = device->context;
    devices[device_count].read = device->read;
    devices[device_count].write = device->write;
    device_count++;
    return true;
}

uint32_t block_device_count(void)
{
    return device_count;
}

const block_device_t *block_device_get(uint32_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

bool block_read(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    void *buffer
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !buffer || !count ||
        lba >= device->sector_count ||
        count > device->sector_count - lba)
        return false;
    return device->read(device->context, lba, count, buffer);
}

bool block_write(
    uint32_t device_index,
    uint64_t lba,
    uint32_t count,
    const void *buffer
)
{
    const block_device_t *device = block_device_get(device_index);
    if (!device || !buffer || !count || !device->writable ||
        !device->write || lba >= device->sector_count ||
        count > device->sector_count - lba)
        return false;
    return device->write(device->context, lba, count, buffer);
}

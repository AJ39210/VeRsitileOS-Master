#ifndef AR5007EG_H
#define AR5007EG_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define AR5007EG_VENDOR_ID 0x168C
#define AR5007EG_DEVICE_ID 0x001C

typedef struct
{
    uint64_t mmio_physical;
    uint64_t mmio_size;
    volatile uint8_t *mmio;
} ar5007eg_resources_t;

static inline bool ar5007eg_matches_device(const pci_device_t *device)
{
    return device &&
        device->vendor_id == AR5007EG_VENDOR_ID &&
        device->device_id == AR5007EG_DEVICE_ID &&
        device->class_code == 0x02;
}

bool ar5007eg_probe(void);
bool ar5007eg_prepare_device(void);
bool ar5007eg_resources_get(ar5007eg_resources_t *resources);
uint32_t ar5007eg_device_count(void);
const pci_device_t *ar5007eg_device_get(uint32_t index);

#endif

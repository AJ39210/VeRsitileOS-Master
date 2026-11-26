#ifndef NVIDIA_8400GS_H
#define NVIDIA_8400GS_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define NVIDIA_8400GS_DEVICE_LIMIT 8

bool nvidia_8400gs_matches_device(const pci_device_t *device);
bool nvidia_8400gs_probe(void);
uint32_t nvidia_8400gs_device_count(void);
const pci_device_t *nvidia_8400gs_device_get(uint32_t index);

#endif

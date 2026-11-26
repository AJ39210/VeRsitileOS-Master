#ifndef INTEL_HD2000_H
#define INTEL_HD2000_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define INTEL_HD2000_DEVICE_LIMIT 8

bool intel_hd2000_matches_device(const pci_device_t *device);
bool intel_hd2000_probe(void);
uint32_t intel_hd2000_device_count(void);
const pci_device_t *intel_hd2000_device_get(uint32_t index);

#endif

#ifndef XHCI_H
#define XHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

typedef struct
{
    const pci_device_t *pci_device;
    uint64_t register_base;
    uint16_t version;
    uint8_t capability_length;
    uint8_t max_slots;
    uint8_t max_interrupters;
    uint8_t max_ports;
} xhci_controller_info_t;

bool xhci_matches_device(const pci_device_t *device);
bool xhci_probe(void);
void xhci_poll(void);
uint32_t xhci_controller_count(void);
const xhci_controller_info_t *xhci_controller_get(uint32_t index);

#endif

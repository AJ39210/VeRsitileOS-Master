#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

struct virtio_legacy_queue;

#define VIRTIO_PCI_DEVICE_LIMIT 32
#define VIRTIO_PCI_CAPABILITY_LIMIT 8

typedef struct
{
    const pci_device_t *pci_device;
    uint16_t device_type;
    bool legacy_io;
    uint16_t io_base;
    bool modern;
    volatile uint8_t *common_config;
    volatile uint8_t *notify_config;
    volatile uint8_t *device_config;
    uint32_t notify_multiplier;
    uint32_t notify_length;
    uint8_t capability_count;
    struct
    {
        uint8_t type;
        uint8_t bar;
        uint32_t offset;
        uint32_t length;
        uint32_t notify_multiplier;
    } capabilities[VIRTIO_PCI_CAPABILITY_LIMIT];
} virtio_pci_device_t;

uint32_t virtio_pci_device_count(void);
const virtio_pci_device_t *virtio_pci_device_get(uint32_t index);
const virtio_pci_device_t *virtio_pci_find_device(
    uint16_t device_type,
    uint32_t index
);
const pci_device_t *virtio_pci_find_legacy(
    uint16_t device_type,
    uint32_t index,
    uint16_t *io_base
);
bool virtio_pci_enable_device(const pci_device_t *device);
bool virtio_pci_modern_init(
    const virtio_pci_device_t *device,
    uint32_t accepted_features_low,
    uint32_t accepted_features_high,
    uint32_t *device_features_low,
    uint32_t *device_features_high
);
bool virtio_pci_modern_queue_init(
    const virtio_pci_device_t *device,
    uint16_t index,
    struct virtio_legacy_queue *queue
);
bool virtio_pci_modern_driver_ok(const virtio_pci_device_t *device);
void virtio_pci_modern_notify(
    const virtio_pci_device_t *device,
    uint16_t queue_index
);

#endif

#ifndef RADEONHD_H
#define RADEONHD_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define RADEONHD_DEVICE_LIMIT 8

typedef enum
{
    RADEONHD_GENERATION_NONE,
    RADEONHD_GENERATION_EVERGREEN,
    RADEONHD_GENERATION_NORTHERN_ISLANDS
} radeonhd_generation_t;

typedef struct
{
    const pci_device_t *pci_device;
    radeonhd_generation_t generation;
    const char *generation_name;
    bool mmio_mapped;
    bool scanout_active;
    bool drm_attached;
    uint32_t scanout_width;
    uint32_t scanout_height;
} radeonhd_device_t;

typedef struct
{
    uint32_t pci_functions_seen;
    uint32_t display_controllers_seen;
    uint32_t amd_display_controllers_seen;
    uint32_t supported_devices;
} radeonhd_probe_stats_t;

bool radeonhd_init(void);
bool radeonhd_is_initialized(void);
uint32_t radeonhd_device_count(void);
bool radeonhd_get_probe_stats(radeonhd_probe_stats_t *stats);
const radeonhd_device_t *radeonhd_device_get(uint32_t index);
const radeonhd_device_t *radeonhd_find_device(
    const pci_device_t *pci_device
);
radeonhd_generation_t radeonhd_classify_device(
    uint16_t vendor_id,
    uint16_t device_id
);
const char *radeonhd_generation_name(radeonhd_generation_t generation);
size_t radeonhd_format_attribute(
    const pci_device_t *pci_device,
    const char *attribute,
    char *buffer,
    size_t capacity
);
size_t radeonhd_format_debug(
    const pci_device_t *pci_device,
    char *buffer,
    size_t capacity
);

#endif

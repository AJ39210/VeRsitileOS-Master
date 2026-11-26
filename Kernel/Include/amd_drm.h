#ifndef AMD_DRM_H
#define AMD_DRM_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

typedef struct
{
    bool mmio_mapped;
    bool scanout_active;
    bool drm_attached;
    uint32_t scanout_width;
    uint32_t scanout_height;
} amd_drm_scanout_info_t;

bool amd_drm_probe_firmware_scanout(
    const pci_device_t *device,
    amd_drm_scanout_info_t *info
);

#endif

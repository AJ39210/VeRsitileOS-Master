#ifndef INTEL_DRM_H
#define INTEL_DRM_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

typedef struct
{
    bool mmio_mapped;
    bool scanout_active;
    bool drm_attached;
    uint32_t pipe;
    uint32_t scanout_width;
    uint32_t scanout_height;
} intel_drm_scanout_info_t;

bool intel_drm_probe_firmware_scanout(
    const pci_device_t *device,
    intel_drm_scanout_info_t *info
);

#endif

#ifndef NVIDIA_DRM_H
#define NVIDIA_DRM_H

#include <stdbool.h>

#include "pci.h"

typedef struct
{
    bool mmio_mapped;
    bool graphics_scanout_selected;
    bool drm_attached;
} nvidia_drm_scanout_info_t;

bool nvidia_drm_probe_firmware_scanout(
    const pci_device_t *device,
    nvidia_drm_scanout_info_t *info
);

#endif

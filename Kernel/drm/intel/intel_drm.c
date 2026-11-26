#include <stdbool.h>
#include <stdint.h>

#include "drm.h"
#include "intel_drm.h"
#include "paging.h"

#define INTEL_DRM_MMIO_MIN_SIZE 0x71020U
#define INTEL_DRM_PIPE_A_CONF 0x70008U
#define INTEL_DRM_PIPE_B_CONF 0x71008U
#define INTEL_DRM_PIPE_A_SRC 0x6001CU
#define INTEL_DRM_PIPE_B_SRC 0x6101CU
#define INTEL_DRM_PIPE_ENABLE (1U << 31)
#define INTEL_DRM_MAX_MODE_DIMENSION 8192U

bool intel_drm_probe_firmware_scanout(
    const pci_device_t *device,
    intel_drm_scanout_info_t *info
)
{
    if (!device || !info)
        return false;
    *info = (intel_drm_scanout_info_t){0};

    pci_bar_info_t bar;
    if (!pci_get_bar_info(device, 0, &bar) ||
        bar.is_io || !bar.base || bar.size < INTEL_DRM_MMIO_MIN_SIZE ||
        !pci_enable_device(device, true, false))
        return false;

    volatile uint8_t *registers = ioremap(
        bar.base, INTEL_DRM_MMIO_MIN_SIZE);
    if (!registers)
        return false;
    info->mmio_mapped = true;

    static const uint32_t pipe_conf[] = {
        INTEL_DRM_PIPE_A_CONF,
        INTEL_DRM_PIPE_B_CONF
    };
    static const uint32_t pipe_src[] = {
        INTEL_DRM_PIPE_A_SRC,
        INTEL_DRM_PIPE_B_SRC
    };
    for (uint32_t pipe = 0; pipe < 2; pipe++)
    {
        if (!(*(volatile uint32_t *)(registers + pipe_conf[pipe]) &
              INTEL_DRM_PIPE_ENABLE))
            continue;

        uint32_t source =
            *(volatile uint32_t *)(registers + pipe_src[pipe]);
        uint32_t width = (source & 0xFFFFU) + 1;
        uint32_t height = (source >> 16) + 1;
        if (!width || !height ||
            width > INTEL_DRM_MAX_MODE_DIMENSION ||
            height > INTEL_DRM_MAX_MODE_DIMENSION)
            continue;

        info->pipe = pipe;
        info->scanout_width = width;
        info->scanout_height = height;
        info->scanout_active = true;
        info->drm_attached = drm_register_firmware_scanout(
            "intel-drm0", width, height);
        return true;
    }

    return true;
}

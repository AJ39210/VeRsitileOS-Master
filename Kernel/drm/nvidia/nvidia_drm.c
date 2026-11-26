#include <stdbool.h>
#include <stdint.h>

#include "drm.h"
#include "nvidia_drm.h"
#include "paging.h"
#include "vesa.h"

#define NVIDIA_DRM_MMIO_MIN_SIZE 0x00600808U
#define NVIDIA_DRM_PCRTC_CONFIG 0x00600804U
#define NVIDIA_DRM_START_ADDRESS_NON_VGA (1U << 0)
#define NVIDIA_DRM_MAX_MODE_DIMENSION 8192U

bool nvidia_drm_probe_firmware_scanout(
    const pci_device_t *device,
    nvidia_drm_scanout_info_t *info
)
{
    if (!device || !info)
        return false;
    *info = (nvidia_drm_scanout_info_t){0};

    pci_bar_info_t bar;
    if (!pci_get_bar_info(device, 0, &bar) ||
        bar.is_io || !bar.base || bar.size < NVIDIA_DRM_MMIO_MIN_SIZE ||
        !pci_enable_device(device, true, false))
        return false;

    volatile uint8_t *registers = ioremap(
        bar.base, NVIDIA_DRM_MMIO_MIN_SIZE);
    if (!registers)
        return false;
    info->mmio_mapped = true;

    uint32_t config =
        *(volatile uint32_t *)(registers + NVIDIA_DRM_PCRTC_CONFIG);
    if (!(config & NVIDIA_DRM_START_ADDRESS_NON_VGA))
        return true;
    info->graphics_scanout_selected = true;

    uint64_t width = vesa_get_width();
    uint64_t height = vesa_get_height();
    if (!width || !height ||
        width > NVIDIA_DRM_MAX_MODE_DIMENSION ||
        height > NVIDIA_DRM_MAX_MODE_DIMENSION)
        return true;

    info->drm_attached = drm_register_firmware_scanout(
        "nvidia-drm0", (uint32_t)width, (uint32_t)height);
    return true;
}

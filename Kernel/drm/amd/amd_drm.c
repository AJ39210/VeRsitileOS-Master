#include <stdbool.h>
#include <stdint.h>

#include "amd_drm.h"
#include "drm.h"
#include "paging.h"

#define AMD_DRM_MMIO_MIN_SIZE 0x608CU
#define AMD_DRM_CRTC_H_TOTAL_DISP 0x6000U
#define AMD_DRM_CRTC_V_TOTAL_DISP 0x6008U
#define AMD_DRM_CRTC_CONTROL 0x6080U
#define AMD_DRM_CRTC_ENABLE (1U << 0)
#define AMD_DRM_DISPLAY_MASK 0x1FFFU
#define AMD_DRM_DISPLAY_SHIFT 16U
#define AMD_DRM_MAX_MODE_DIMENSION 8192U

bool amd_drm_probe_firmware_scanout(
    const pci_device_t *device,
    amd_drm_scanout_info_t *info
)
{
    if (!device || !info)
        return false;
    *info = (amd_drm_scanout_info_t){0};

    pci_bar_info_t bar;
    if (!pci_get_bar_info(device, 0, &bar) ||
        bar.is_io || !bar.base || bar.size < AMD_DRM_MMIO_MIN_SIZE ||
        !pci_enable_device(device, true, false))
        return false;

    volatile uint8_t *registers = ioremap(
        bar.base, AMD_DRM_MMIO_MIN_SIZE);
    if (!registers)
        return false;
    info->mmio_mapped = true;

    uint32_t control =
        *(volatile uint32_t *)(registers + AMD_DRM_CRTC_CONTROL);
    uint32_t horizontal =
        *(volatile uint32_t *)(registers + AMD_DRM_CRTC_H_TOTAL_DISP);
    uint32_t vertical =
        *(volatile uint32_t *)(registers + AMD_DRM_CRTC_V_TOTAL_DISP);
    if (!(control & AMD_DRM_CRTC_ENABLE) ||
        horizontal == UINT32_MAX || vertical == UINT32_MAX)
        return true;

    uint32_t width =
        ((horizontal >> AMD_DRM_DISPLAY_SHIFT) & AMD_DRM_DISPLAY_MASK) + 1;
    uint32_t height =
        ((vertical >> AMD_DRM_DISPLAY_SHIFT) & AMD_DRM_DISPLAY_MASK) + 1;
    if (!width || !height ||
        width > AMD_DRM_MAX_MODE_DIMENSION ||
        height > AMD_DRM_MAX_MODE_DIMENSION)
        return true;

    info->scanout_width = width;
    info->scanout_height = height;
    info->scanout_active = true;
    info->drm_attached = drm_register_firmware_scanout(
        "radeonhd-drm0", width, height);
    return true;
}

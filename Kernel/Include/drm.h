#ifndef DRM_H
#define DRM_H

#include <stdbool.h>
#include <stdint.h>

#define DRM_DEVICE_NAME_MAX 16
#define DRM_FRAMEBUFFER_LIMIT 4
#define DRM_MAX_FRAMEBUFFER_BYTES (64ULL * 1024 * 1024)

typedef enum
{
    DRM_FORMAT_XRGB8888 = 1
} drm_pixel_format_t;

typedef struct
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    bool valid;
} drm_damage_t;

typedef struct
{
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    drm_pixel_format_t format;
    uint32_t *pixels;
    drm_damage_t damage;
    bool allocated;
} drm_framebuffer_t;

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t refresh_rate_hz;
    drm_pixel_format_t format;
} drm_mode_info_t;

typedef struct
{
    char name[DRM_DEVICE_NAME_MAX];
    uint32_t connector_count;
    uint32_t crtc_count;
    uint32_t framebuffer_count;
    uint64_t present_count;
    bool connected;
    bool modesetting;
    bool hardware_acceleration;
    bool supports_software_framebuffers;
    bool supports_partial_updates;
    drm_mode_info_t mode;
} drm_device_info_t;

bool drm_init(void);
bool drm_is_initialized(void);
bool drm_get_device_info(drm_device_info_t *info);
bool drm_register_firmware_scanout(
    const char *name,
    uint32_t width,
    uint32_t height
);
bool drm_framebuffer_create(
    uint32_t width,
    uint32_t height,
    drm_pixel_format_t format,
    drm_framebuffer_t **framebuffer
);
void drm_framebuffer_destroy(drm_framebuffer_t *framebuffer);
bool drm_framebuffer_clear(
    drm_framebuffer_t *framebuffer,
    uint32_t color
);
bool drm_framebuffer_set_pixel(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t color
);
bool drm_framebuffer_fill_rect(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
);
bool drm_framebuffer_blit(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint64_t stride
);
bool drm_framebuffer_mark_dirty(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);
bool drm_framebuffer_present(drm_framebuffer_t *framebuffer);
bool drm_framebuffer_present_region(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);
bool drm_fill_rect(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t color
);
bool drm_draw_text(
    uint64_t x,
    uint64_t y,
    uint32_t color,
    const char *text,
    uint64_t length
);
bool drm_blit(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    const uint32_t *pixels,
    uint64_t stride
);
bool drm_read_pixels(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t *pixels,
    uint64_t stride
);

#endif

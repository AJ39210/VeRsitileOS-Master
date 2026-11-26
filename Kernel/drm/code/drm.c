#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "heap.h"
#include "vesa.h"

static drm_device_info_t display_device;
static drm_framebuffer_t framebuffers[DRM_FRAMEBUFFER_LIMIT];
static uint32_t next_framebuffer_id = 1;
static uint64_t present_count;
static bool initialized;

static bool framebuffer_is_valid(const drm_framebuffer_t *framebuffer)
{
    if (!framebuffer)
        return false;
    for (uint32_t i = 0; i < DRM_FRAMEBUFFER_LIMIT; i++)
    {
        if (&framebuffers[i] == framebuffer)
            return framebuffer->allocated &&
                framebuffer->pixels && framebuffer->id;
    }
    return false;
}

static void framebuffer_add_damage(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (!framebuffer->damage.valid)
    {
        framebuffer->damage = (drm_damage_t) {
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .valid = true
        };
        return;
    }

    uint32_t left = x < framebuffer->damage.x ? x : framebuffer->damage.x;
    uint32_t top = y < framebuffer->damage.y ? y : framebuffer->damage.y;
    uint32_t right_a = x + width;
    uint32_t right_b = framebuffer->damage.x + framebuffer->damage.width;
    uint32_t bottom_a = y + height;
    uint32_t bottom_b = framebuffer->damage.y + framebuffer->damage.height;
    uint32_t right = right_a > right_b ? right_a : right_b;
    uint32_t bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
    framebuffer->damage.x = left;
    framebuffer->damage.y = top;
    framebuffer->damage.width = right - left;
    framebuffer->damage.height = bottom - top;
}

static bool clip_rectangle(
    const drm_framebuffer_t *framebuffer,
    uint32_t *x,
    uint32_t *y,
    uint32_t *width,
    uint32_t *height
)
{
    if (!framebuffer_is_valid(framebuffer) || !x || !y ||
        !width || !height || !*width || !*height ||
        *x >= framebuffer->width || *y >= framebuffer->height)
        return false;
    if (*width > framebuffer->width - *x)
        *width = framebuffer->width - *x;
    if (*height > framebuffer->height - *y)
        *height = framebuffer->height - *y;
    return true;
}

bool drm_init(void)
{
    uint64_t width = vesa_get_width();
    uint64_t height = vesa_get_height();
    uint64_t pitch_bytes = vesa_get_pitch_bytes();
    if (!width || !height || width > UINT32_MAX ||
        height > UINT32_MAX || pitch_bytes > UINT32_MAX)
        return false;

    display_device = (drm_device_info_t) {
        .name = "vesa-drm0",
        .connector_count = 1,
        .crtc_count = 1,
        .framebuffer_count = 1,
        .connected = true,
        .modesetting = false,
        .hardware_acceleration = false,
        .supports_software_framebuffers = true,
        .supports_partial_updates = true,
        .mode = {
            .width = (uint32_t)width,
            .height = (uint32_t)height,
            .pitch_bytes = (uint32_t)pitch_bytes,
            .refresh_rate_hz = 0,
            .format = DRM_FORMAT_XRGB8888
        }
    };
    initialized = true;
    return true;
}

bool drm_is_initialized(void)
{
    return initialized;
}

bool drm_get_device_info(drm_device_info_t *info)
{
    if (!initialized || !info)
        return false;

    uint32_t framebuffer_count = 1;
    for (uint32_t i = 0; i < DRM_FRAMEBUFFER_LIMIT; i++)
    {
        if (framebuffers[i].allocated)
            framebuffer_count++;
    }
    *info = display_device;
    info->framebuffer_count = framebuffer_count;
    info->present_count = present_count;
    return true;
}

bool drm_register_firmware_scanout(
    const char *name,
    uint32_t width,
    uint32_t height
)
{
    if (!initialized || !name || !width || !height ||
        width != display_device.mode.width ||
        height != display_device.mode.height)
        return false;

    size_t length = 0;
    while (length < sizeof(display_device.name) && name[length])
        length++;
    if (!length || length == sizeof(display_device.name))
        return false;

    for (size_t i = 0; i <= length; i++)
        display_device.name[i] = name[i];
    display_device.connected = true;
    display_device.modesetting = false;
    display_device.hardware_acceleration = false;
    return true;
}

bool drm_framebuffer_create(
    uint32_t width,
    uint32_t height,
    drm_pixel_format_t format,
    drm_framebuffer_t **result
)
{
    if (!result)
        return false;
    *result = NULL;
    if (!initialized || !width || !height ||
        format != DRM_FORMAT_XRGB8888 ||
        width > UINT32_MAX / sizeof(uint32_t))
        return false;

    uint64_t pitch_bytes = (uint64_t)width * sizeof(uint32_t);
    uint64_t allocation_size = pitch_bytes * height;
    if (allocation_size > DRM_MAX_FRAMEBUFFER_BYTES)
        return false;

    drm_framebuffer_t *slot = NULL;
    for (uint32_t i = 0; i < DRM_FRAMEBUFFER_LIMIT; i++)
    {
        if (!framebuffers[i].allocated)
        {
            slot = &framebuffers[i];
            break;
        }
    }
    if (!slot)
        return false;

    uint32_t *pixels = kcalloc(1, allocation_size);
    if (!pixels)
        return false;

    uint32_t id = next_framebuffer_id++;
    if (!id)
        id = next_framebuffer_id++;
    *slot = (drm_framebuffer_t) {
        .id = id,
        .width = width,
        .height = height,
        .pitch_bytes = (uint32_t)pitch_bytes,
        .format = format,
        .pixels = pixels,
        .allocated = true,
        .damage = {
            .x = 0,
            .y = 0,
            .width = width,
            .height = height,
            .valid = true
        }
    };
    *result = slot;
    return true;
}

void drm_framebuffer_destroy(drm_framebuffer_t *framebuffer)
{
    if (!framebuffer_is_valid(framebuffer))
        return;
    kfree(framebuffer->pixels);
    *framebuffer = (drm_framebuffer_t){0};
}

bool drm_framebuffer_clear(
    drm_framebuffer_t *framebuffer,
    uint32_t color
)
{
    if (!framebuffer_is_valid(framebuffer))
        return false;
    uint64_t pixel_count = (uint64_t)framebuffer->width * framebuffer->height;
    for (uint64_t i = 0; i < pixel_count; i++)
        framebuffer->pixels[i] = color & 0x00FFFFFFU;
    framebuffer_add_damage(
        framebuffer,
        0,
        0,
        framebuffer->width,
        framebuffer->height);
    return true;
}

bool drm_framebuffer_set_pixel(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    if (!framebuffer_is_valid(framebuffer) ||
        x >= framebuffer->width || y >= framebuffer->height)
        return false;
    uint32_t pitch = framebuffer->pitch_bytes / sizeof(uint32_t);
    framebuffer->pixels[(uint64_t)y * pitch + x] = color & 0x00FFFFFFU;
    framebuffer_add_damage(framebuffer, x, y, 1, 1);
    return true;
}

bool drm_framebuffer_fill_rect(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
)
{
    if (!clip_rectangle(framebuffer, &x, &y, &width, &height))
        return false;

    uint32_t pitch = framebuffer->pitch_bytes / sizeof(uint32_t);
    color &= 0x00FFFFFFU;
    for (uint32_t row = 0; row < height; row++)
    {
        uint32_t *pixels = framebuffer->pixels +
            (uint64_t)(y + row) * pitch + x;
        for (uint32_t column = 0; column < width; column++)
            pixels[column] = color;
    }
    framebuffer_add_damage(framebuffer, x, y, width, height);
    return true;
}

bool drm_framebuffer_blit(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint64_t stride
)
{
    if (!pixels || stride < width ||
        (height > 1 && stride > UINT64_MAX / (height - 1)) ||
        !clip_rectangle(framebuffer, &x, &y, &width, &height))
        return false;

    uint32_t pitch = framebuffer->pitch_bytes / sizeof(uint32_t);
    for (uint32_t row = 0; row < height; row++)
    {
        uint32_t *destination = framebuffer->pixels +
            (uint64_t)(y + row) * pitch + x;
        const uint32_t *source = pixels + (uint64_t)row * stride;
        for (uint32_t column = 0; column < width; column++)
            destination[column] = source[column] & 0x00FFFFFFU;
    }
    framebuffer_add_damage(framebuffer, x, y, width, height);
    return true;
}

bool drm_framebuffer_mark_dirty(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (!clip_rectangle(framebuffer, &x, &y, &width, &height))
        return false;
    framebuffer_add_damage(framebuffer, x, y, width, height);
    return true;
}

bool drm_framebuffer_present_region(
    drm_framebuffer_t *framebuffer,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (!clip_rectangle(framebuffer, &x, &y, &width, &height) ||
        framebuffer->width != display_device.mode.width ||
        framebuffer->height != display_device.mode.height)
        return false;

    uint32_t pitch = framebuffer->pitch_bytes / sizeof(uint32_t);
    if (!drm_blit(
            x,
            y,
            width,
            height,
            framebuffer->pixels + (uint64_t)y * pitch + x,
            pitch))
        return false;

    present_count++;
    return true;
}

bool drm_framebuffer_present(drm_framebuffer_t *framebuffer)
{
    if (!framebuffer_is_valid(framebuffer))
        return false;
    if (!framebuffer->damage.valid)
        return true;

    drm_damage_t damage = framebuffer->damage;
    if (!drm_framebuffer_present_region(
            framebuffer,
            damage.x,
            damage.y,
            damage.width,
            damage.height))
        return false;
    framebuffer->damage.valid = false;
    return true;
}

bool drm_fill_rect(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t color
)
{
    if (!initialized)
        return false;
    vesa_fill_rect(x, y, width, height, color);
    return true;
}

bool drm_draw_text(
    uint64_t x,
    uint64_t y,
    uint32_t color,
    const char *text,
    uint64_t length
)
{
    if (!initialized || (!text && length))
        return false;
    vesa_draw_text(x, y, color, text, length);
    return true;
}

bool drm_blit(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    const uint32_t *pixels,
    uint64_t stride
)
{
    return initialized &&
        vesa_blit_rgb(x, y, width, height, pixels, stride);
}

bool drm_read_pixels(
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint32_t *pixels,
    uint64_t stride
)
{
    return initialized &&
        vesa_read_rgb(x, y, width, height, pixels, stride);
}

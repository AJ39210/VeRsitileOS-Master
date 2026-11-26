#ifndef LIBX_TEST_OS_GUI_H
#define LIBX_TEST_OS_GUI_H

#include <stdint.h>

typedef struct
{
    uint64_t width;
    uint64_t height;
} os_screen_info_t;

typedef struct
{
    uint32_t type;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t text_length;
    char text[192];
} os_draw_command_t;

typedef struct
{
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    uint8_t reserved[3];
} os_mouse_event_t;

typedef struct
{
    uint8_t scancode;
    uint8_t character;
    uint8_t pressed;
    uint8_t shift;
    uint8_t caps_lock;
    uint8_t extended;
} os_keyboard_event_t;

typedef struct
{
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    const uint32_t *pixels;
} os_screen_region_t;

enum
{
    OS_DRAW_RECTANGLE = 1,
    OS_DRAW_TEXT = 2
};

int64_t os_screen_info(os_screen_info_t *info);
int64_t os_screen_blit(const os_screen_region_t *region);
int64_t os_screen_read(os_screen_region_t *region);
int64_t os_draw_batch(const os_draw_command_t *commands, uint64_t count);
int64_t os_mouse_read(os_mouse_event_t *events, uint64_t size);
int64_t os_keyboard_read(os_keyboard_event_t *events, uint64_t size);

#endif

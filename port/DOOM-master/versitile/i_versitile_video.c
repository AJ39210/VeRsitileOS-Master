#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <os/gui.h>

#include "doomdef.h"
#include "d_event.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

static uint32_t palette[256];
static uint32_t *framebuffer;
static uint64_t screen_width;
static uint64_t screen_height;
static uint32_t output_width;
static uint32_t output_height;
static uint64_t screen_x;
static uint64_t screen_y;

static int key_from_scancode(const os_keyboard_event_t *key)
{
    if (key->extended)
    {
        switch (key->scancode)
        {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1D: return KEY_RCTRL;
            case 0x38: return KEY_RALT;
            default: return -1;
        }
    }

    switch (key->scancode)
    {
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_RCTRL;
        case 0x2A:
        case 0x36: return KEY_RSHIFT;
        case 0x38: return KEY_RALT;
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:
            break;
    }

    if (key->character)
    {
        int character = key->character;
        if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
        return character;
    }

    static const char scan_map[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3',
        [0x05] = '4', [0x06] = '5', [0x07] = '6',
        [0x08] = '7', [0x09] = '8', [0x0A] = '9',
        [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
        [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
        [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
        [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
        [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
        [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
        [0x27] = ';', [0x28] = '\'', [0x29] = '`',
        [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
        [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
        [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
        [0x34] = '.', [0x35] = '/', [0x39] = ' '
    };
    return scan_map[key->scancode] ? scan_map[key->scancode] : -1;
}

void I_InitGraphics(void)
{
    os_screen_info_t screen;
    if (os_screen_info(&screen) < 0 || !screen.width || !screen.height)
        I_Error("no framebuffer is available");

    screen_width = screen.width;
    screen_height = screen.height;
    uint64_t scale_x = screen_width / SCREENWIDTH;
    uint64_t scale_y = screen_height / SCREENHEIGHT;
    uint64_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (!scale)
        scale = 1;
    if (scale > 3)
        scale = 3;

    output_width = (uint32_t)(SCREENWIDTH * scale);
    output_height = (uint32_t)(SCREENHEIGHT * scale);
    if (output_width > screen_width)
        output_width = (uint32_t)screen_width;
    if (output_height > screen_height)
        output_height = (uint32_t)screen_height;
    if ((uint64_t)output_width * output_height >
        16ULL * 1024 * 1024)
        I_Error("framebuffer dimensions exceed the supported limit");

    framebuffer = (uint32_t *)malloc(
        (size_t)output_width * output_height * sizeof(*framebuffer));
    if (!framebuffer)
        I_Error("could not allocate framebuffer");
    screen_x = (screen_width - output_width) / 2;
    screen_y = (screen_height - output_height) / 2;
    (void)os_console_clear();
}

void I_ShutdownGraphics(void)
{
    free(framebuffer);
    framebuffer = NULL;
}

void I_SetPalette(byte *colors)
{
    if (!colors)
        return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t red = colors[i * 3];
        uint32_t green = colors[i * 3 + 1];
        uint32_t blue = colors[i * 3 + 2];
        palette[i] = (red << 16) | (green << 8) | blue;
    }
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
    os_keyboard_event_t keys[32];
    int64_t bytes = os_keyboard_read(keys, sizeof(keys));
    if ((bytes < 0 && bytes != -EAGAIN) ||
        (bytes > 0 && (uint64_t)bytes % sizeof(keys[0]) != 0))
        I_Error("keyboard input failed");
    if (bytes > 0)
    {
        uint64_t count = (uint64_t)bytes / sizeof(keys[0]);
        for (uint64_t i = 0; i < count; i++)
        {
            int code = key_from_scancode(&keys[i]);
            if (code < 0)
                continue;
            event_t event = {
                .type = keys[i].pressed ? ev_keydown : ev_keyup,
                .data1 = code,
                .data2 = 0,
                .data3 = 0
            };
            D_PostEvent(&event);
        }
    }

    os_mouse_event_t mouse[16];
    bytes = os_mouse_read(mouse, sizeof(mouse));
    if ((bytes < 0 && bytes != -EAGAIN && bytes != -ENODEV) ||
        (bytes > 0 && (uint64_t)bytes % sizeof(mouse[0]) != 0))
        I_Error("mouse input failed");
    if (bytes > 0)
    {
        uint64_t count = (uint64_t)bytes / sizeof(mouse[0]);
        for (uint64_t i = 0; i < count; i++)
        {
            event_t event = {
                .type = ev_mouse,
                .data1 = mouse[i].buttons & 7,
                .data2 = (int)mouse[i].dx * 4,
                .data3 = -(int)mouse[i].dy * 4
            };
            D_PostEvent(&event);
        }
    }
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    if (!framebuffer || !screens[0])
        return;
    for (uint32_t y = 0; y < output_height; y++)
    {
        uint32_t source_y =
            (uint64_t)y * SCREENHEIGHT / output_height;
        uint32_t *destination =
            framebuffer + (size_t)y * output_width;
        const byte *source =
            screens[0] + (size_t)source_y * SCREENWIDTH;
        for (uint32_t x = 0; x < output_width; x++)
        {
            uint32_t source_x =
                (uint64_t)x * SCREENWIDTH / output_width;
            destination[x] = palette[source[source_x]];
        }
    }

    os_screen_region_t region = {
        .x = (uint64_t)screen_x,
        .y = (uint64_t)screen_y,
        .width = output_width,
        .height = output_height,
        .stride = output_width,
        .pixels = framebuffer
    };
    if (os_screen_blit(&region) < 0)
        I_Error("framebuffer blit failed");
}

void I_ReadScreen(byte *screen)
{
    memcpy(screen, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

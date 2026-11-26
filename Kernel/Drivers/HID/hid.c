#include <stdbool.h>
#include <stdint.h>

#include "hid.h"
#include "keyboard.h"
#include "mouse.h"

static uint8_t previous_modifiers;
static uint8_t previous_keys[6];

static uint8_t keyboard_scancode(uint8_t usage)
{
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,
        0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
        0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
        0x15, 0x2C
    };
    if (usage >= 0x04 && usage <= 0x1D)
        return letters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x26)
        return (uint8_t)(0x02 + usage - 0x1E);
    if (usage == 0x27)
        return 0x0B;

    switch (usage)
    {
        case 0x28: return 0x1C;
        case 0x29: return 0x01;
        case 0x2A: return 0x0E;
        case 0x2B: return 0x0F;
        case 0x2C: return 0x39;
        case 0x2D: return 0x0C;
        case 0x2E: return 0x0D;
        case 0x2F: return 0x1A;
        case 0x30: return 0x1B;
        case 0x31: return 0x2B;
        case 0x33: return 0x27;
        case 0x34: return 0x28;
        case 0x35: return 0x29;
        case 0x36: return 0x33;
        case 0x37: return 0x34;
        case 0x38: return 0x35;
        case 0x39: return 0x3A;
        case 0x3A: return 0x3B;
        case 0x3B: return 0x3C;
        case 0x3C: return 0x3D;
        case 0x3D: return 0x3E;
        case 0x3E: return 0x3F;
        case 0x3F: return 0x40;
        case 0x40: return 0x41;
        case 0x41: return 0x42;
        case 0x42: return 0x43;
        case 0x43: return 0x44;
        case 0x44: return 0x57;
        case 0x46: return 0x37;
        default: return 0;
    }
}

static bool report_has_key(const uint8_t report[6], uint8_t usage)
{
    for (uint32_t i = 0; i < 6; i++)
    {
        if (report[i] == usage)
            return true;
    }
    return false;
}

void hid_init(void)
{
    previous_modifiers = 0;
    for (uint32_t i = 0; i < sizeof(previous_keys); i++)
        previous_keys[i] = 0;
}

bool hid_handle_boot_keyboard_report(const uint8_t report[8])
{
    if (!report)
        return false;

    const uint8_t *keys = report + 2;
    for (uint32_t i = 0; i < 6; i++)
    {
        if (keys[i] >= 1 && keys[i] <= 3)
            return false;
    }

    static const uint8_t modifier_scancodes[8] = {
        0x1D, 0x2A, 0x38, 0x5B, 0x1D, 0x36, 0x38, 0x5C
    };
    for (uint8_t i = 0; i < 8; i++)
    {
        bool was_pressed = (previous_modifiers & (1U << i)) != 0;
        bool is_pressed = (report[0] & (1U << i)) != 0;
        if (was_pressed == is_pressed)
            continue;
        if (i == 3 || i >= 4)
            keyboard_handle_scancode(0xE0);
        keyboard_handle_scancode(
            (uint8_t)(modifier_scancodes[i] |
                (is_pressed ? 0 : 0x80)));
    }

    for (uint32_t i = 0; i < 6; i++)
    {
        uint8_t old_usage = previous_keys[i];
        if (old_usage && !report_has_key(keys, old_usage))
        {
            uint8_t scancode = keyboard_scancode(old_usage);
            if (scancode)
            {
                if (old_usage == 0x46)
                    keyboard_handle_scancode(0xE0);
                keyboard_handle_scancode(scancode | 0x80);
            }
        }
    }
    for (uint32_t i = 0; i < 6; i++)
    {
        uint8_t usage = keys[i];
        if (usage && !report_has_key(previous_keys, usage))
        {
            uint8_t scancode = keyboard_scancode(usage);
            if (scancode)
            {
                if (usage == 0x46)
                    keyboard_handle_scancode(0xE0);
                keyboard_handle_scancode(scancode);
            }
        }
    }

    previous_modifiers = report[0];
    for (uint32_t i = 0; i < sizeof(previous_keys); i++)
        previous_keys[i] = keys[i];
    return true;
}

bool hid_handle_boot_mouse_report(const uint8_t report[3])
{
    if (!report)
        return false;
    mouse_handle_usb_report(
        (int8_t)report[1],
        (int8_t)report[2],
        report[0] & 0x07);
    return true;
}


#include "keyboard.h"
#include "ps2.h"
#include "xhci.h"
#include "virtio_input.h"
#include "panic.h"

/*
 * PS/2 Set 1 keyboard driver.
 * This file does not activate itself.
 */

#define KEYBOARD_BUFFER_SIZE 256
#define KEYBOARD_EVENT_BUFFER_SIZE 2048

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static keyboard_event_t keyboard_event_buffer[KEYBOARD_EVENT_BUFFER_SIZE];

static uint16_t buffer_head = 0;
static uint16_t buffer_tail = 0;
static uint16_t event_head = 0;
static uint16_t event_tail = 0;

static bool left_shift = false;
static bool right_shift = false;
static bool left_control = false;
static bool right_control = false;
static bool caps_lock = false;
static bool sysrq_pressed = false;
static bool extended = false;

static bool shift_active(void)
{
    return left_shift || right_shift;
}

static bool control_active(void)
{
    return left_control || right_control;
}

/* US QWERTY layout */
static const char normal_map[128] =
{
    [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0',

    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',

    [0x10] = 'q', [0x11] = 'w',
    [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',

    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l',

    [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2B] = '\\',

    [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n',
    [0x32] = 'm',

    [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' '
};

static const char shift_map[128] =
{
    [0x02] = '!', [0x03] = '@',
    [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')',

    [0x0C] = '_', [0x0D] = '+',

    [0x10] = 'Q', [0x11] = 'W',
    [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P',

    [0x1A] = '{', [0x1B] = '}',

    [0x1E] = 'A', [0x1F] = 'S',
    [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L',

    [0x27] = ':', [0x28] = '"',
    [0x29] = '~', [0x2B] = '|',

    [0x2C] = 'Z', [0x2D] = 'X',
    [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N',
    [0x32] = 'M',

    [0x33] = '<', [0x34] = '>',
    [0x35] = '?'
};

static void buffer_push(char c)
{
    uint16_t next = (buffer_head + 1) % KEYBOARD_BUFFER_SIZE;

    if (next == buffer_tail)
        return;

    keyboard_buffer[buffer_head] = c;
    buffer_head = next;
}

static void event_push(keyboard_event_t event)
{
    uint16_t next = (event_head + 1) % KEYBOARD_EVENT_BUFFER_SIZE;

    if (next == event_tail)
        return;

    keyboard_event_buffer[event_head] = event;
    event_head = next;
}

void keyboard_init(void)
{
    buffer_head = 0;
    buffer_tail = 0;
    event_head = 0;
    event_tail = 0;

    left_shift = false;
    right_shift = false;
    left_control = false;
    right_control = false;
    caps_lock = false;
    sysrq_pressed = false;
    extended = false;
}

void keyboard_handle_scancode(uint8_t scancode)
{
    /* Extended key prefix */
    if (scancode == 0xE0)
    {
        extended = true;
        return;
    }

    bool is_extended = extended;
    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (code == 0x1D)
    {
        if (is_extended)
            right_control = !released;
        else
            left_control = !released;
        extended = false;

        keyboard_event_t event = {
            .scancode = code,
            .character = 0,
            .pressed = !released,
            .shift = shift_active(),
            .caps_lock = caps_lock,
            .extended = is_extended
        };
        event_push(event);
        return;
    }

    if (is_extended && code == 0x37)
    {
        sysrq_pressed = !released;
        extended = false;

        keyboard_event_t event = {
            .scancode = code,
            .character = 0,
            .pressed = !released,
            .shift = shift_active(),
            .caps_lock = caps_lock,
            .extended = true
        };
        event_push(event);
        return;
    }

    if (!is_extended)
    {
        switch (code)
        {
            case 0x2A:
                left_shift = !released;
                break;

            case 0x36:
                right_shift = !released;
                break;

            case 0x3A:
                if (!released)
                    caps_lock = !caps_lock;
                break;

            default:
                goto process_key;
        }

        keyboard_event_t event = {
            .scancode = code,
            .character = 0,
            .pressed = !released,
            .shift = shift_active(),
            .caps_lock = caps_lock,
            .extended = false
        };
        event_push(event);
        return;
    }

process_key:
    if (released)
    {
        extended = false;
        keyboard_event_t event = {
            .scancode = code,
            .character = 0,
            .pressed = false,
            .shift = shift_active(),
            .caps_lock = caps_lock,
            .extended = is_extended
        };
        event_push(event);
        return;
    }

    /* Extended keys are handled separately. */
    if (is_extended)
    {
        extended = false;
        keyboard_event_t event = {
            .scancode = code,
            .character = 0,
            .pressed = true,
            .shift = shift_active(),
            .caps_lock = caps_lock,
            .extended = true
        };
        event_push(event);
        return;
    }

    if (code == 0x25 && control_active() && sysrq_pressed)
        kernel_panic("Ctrl+SysRq+K requested");

    bool shift = shift_active();

    char c = shift ? shift_map[code] : normal_map[code];

    /* Caps Lock changes letters, not numbers or symbols. */
    if (normal_map[code] >= 'a' &&
        normal_map[code] <= 'z' &&
        caps_lock)
    {
        c = shift ? normal_map[code] : shift_map[code];
    }

    keyboard_event_t event = {
        .scancode = code,
        .character = c,
        .pressed = true,
        .shift = shift,
        .caps_lock = caps_lock,
        .extended = false
    };
    event_push(event);

    if (c)
        buffer_push(c);
}

bool keyboard_has_input(void)
{
    keyboard_poll();
    return buffer_head != buffer_tail;
}

void keyboard_poll(void)
{
    ps2_poll();
    xhci_poll();
    virtio_input_poll();
}

char keyboard_getchar(void)
{
    if (!keyboard_has_input())
        return 0;

    char c = keyboard_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;

    return c;
}

int64_t keyboard_read_events(void *buffer, uint64_t size)
{
    if (!buffer || size < sizeof(keyboard_event_t) ||
        size % sizeof(keyboard_event_t) != 0)
        return -1;

    uint64_t capacity = size / sizeof(keyboard_event_t);
    uint64_t count = 0;
    keyboard_event_t *events = buffer;

    while (count < capacity && event_head != event_tail)
    {
        events[count++] = keyboard_event_buffer[event_tail];
        event_tail = (event_tail + 1) % KEYBOARD_EVENT_BUFFER_SIZE;
    }

    return (int64_t)(count * sizeof(keyboard_event_t));
}

bool keyboard_shift_pressed(void)
{
    return shift_active();
}

bool keyboard_caps_lock_enabled(void)
{
    return caps_lock;
}

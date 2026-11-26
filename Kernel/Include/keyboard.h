
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint8_t scancode;
    uint8_t character;
    uint8_t pressed;
    uint8_t shift;
    uint8_t caps_lock;
    uint8_t extended;
} keyboard_event_t;

/* Initialize keyboard state. */
void keyboard_init(void);

/* Process a keyboard scancode. */
void keyboard_handle_scancode(uint8_t scancode);

/* Check whether a character is available. */
bool keyboard_has_input(void);

/* Poll attached USB and VirtIO input devices. */
void keyboard_poll(void);

/* Retrieve the next character. */
char keyboard_getchar(void);

/* Read raw key transitions; returns the number of bytes copied. */
int64_t keyboard_read_events(void *buffer, uint64_t size);

/* Check modifier states. */
bool keyboard_shift_pressed(void);
bool keyboard_caps_lock_enabled(void);

#endif

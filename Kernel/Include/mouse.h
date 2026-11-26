#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    uint8_t reserved[3];
} mouse_event_t;

#define MOUSE_BUTTON_LEFT   (1U << 0)
#define MOUSE_BUTTON_RIGHT  (1U << 1)
#define MOUSE_BUTTON_MIDDLE (1U << 2)

bool mouse_init(void);
bool mouse_is_initialized(void);
int64_t mouse_read_events(void *buffer, uint64_t size);
void mouse_handle_usb_report(int16_t dx, int16_t dy, uint8_t buttons);

#endif

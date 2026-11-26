#ifndef HID_H
#define HID_H

#include <stdbool.h>
#include <stdint.h>

void hid_init(void);
bool hid_handle_boot_keyboard_report(const uint8_t report[8]);
bool hid_handle_boot_mouse_report(const uint8_t report[3]);

#endif

#ifndef E1000_H
#define E1000_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool e1000_init(void);
bool e1000_is_initialized(void);
bool e1000_link_is_up(void);
const uint8_t *e1000_mac_address(void);
bool e1000_send(const void *packet, size_t length);
size_t e1000_receive(void *packet, size_t capacity);

#endif

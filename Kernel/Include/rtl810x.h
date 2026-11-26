#ifndef RTL810X_H
#define RTL810X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool rtl810x_init(void);
bool rtl810x_is_initialized(void);
bool rtl810x_link_is_up(void);
const uint8_t *rtl810x_mac_address(void);
bool rtl810x_send(const void *packet, size_t length);
size_t rtl810x_receive(void *packet, size_t capacity);

static inline uint8_t rtl810x_mac_version(uint32_t tx_config)
{
    uint32_t version = tx_config & 0x7CF00000U;
    if (version == 0x34900000U || version == 0x24900000U)
        return 8;
    if (version == 0x34800000U || version == 0x24800000U)
        return 7;

    version = tx_config & 0x7C800000U;
    if (version == 0x34800000U || version == 0x24800000U)
        return 9;
    return 0;
}

bool rtl8102e_matches_tx_config(uint32_t tx_config);
bool rtl8103e_matches_tx_config(uint32_t tx_config);

bool realtek_is_initialized(void);
bool realtek_link_is_up(void);
const uint8_t *realtek_mac_address(void);
bool realtek_send(const void *packet, size_t length);
size_t realtek_receive(void *packet, size_t capacity);

#endif

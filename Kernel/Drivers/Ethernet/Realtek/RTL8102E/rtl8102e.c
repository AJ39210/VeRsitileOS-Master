#include <stdbool.h>
#include <stdint.h>

#include "rtl810x.h"

bool rtl8102e_matches_tx_config(uint32_t tx_config)
{
    uint8_t version = rtl810x_mac_version(tx_config);
    return version == 7 || version == 8;
}

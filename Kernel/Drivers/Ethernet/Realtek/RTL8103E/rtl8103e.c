#include <stdbool.h>
#include <stdint.h>

#include "rtl810x.h"

bool rtl8103e_matches_tx_config(uint32_t tx_config)
{
    return rtl810x_mac_version(tx_config) == 9;
}

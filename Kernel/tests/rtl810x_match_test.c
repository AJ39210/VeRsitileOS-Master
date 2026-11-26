#include <assert.h>
#include <stdint.h>

#include "rtl810x.h"

int main(void)
{
    assert(rtl810x_mac_version(0x34800600U) == 7);
    assert(rtl810x_mac_version(0x24800000U) == 7);
    assert(rtl810x_mac_version(0x34900000U) == 8);
    assert(rtl810x_mac_version(0x24900600U) == 8);
    assert(rtl810x_mac_version(0x34A00000U) == 9);
    assert(rtl810x_mac_version(0x24A00000U) == 9);
    assert(rtl810x_mac_version(0x34B00000U) == 9);
    assert(rtl810x_mac_version(0x24B00000U) == 9);
    assert(rtl810x_mac_version(0x34000000U) == 0);

    assert(rtl8102e_matches_tx_config(0x34800600U));
    assert(rtl8102e_matches_tx_config(0x34900000U));
    assert(!rtl8102e_matches_tx_config(0x34A00000U));
    assert(rtl8103e_matches_tx_config(0x34A00000U));
    assert(!rtl8103e_matches_tx_config(0x34800000U));
    return 0;
}

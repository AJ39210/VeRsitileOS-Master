#include <stdbool.h>

#include "module.h"
#include "rtl810x.h"
#include "serial.h"

const uint32_t vos_module_abi_version = 1;

bool vos_module_init(void)
{
    const module_ethernet_ops_t ops = {
        .is_initialized = rtl810x_is_initialized,
        .link_is_up = rtl810x_link_is_up,
        .mac_address = rtl810x_mac_address,
        .send = rtl810x_send,
        .receive = rtl810x_receive
    };
    if (!kernel_module_register_realtek(&ops))
    {
        serial_log(
            SERIAL_LOG_ERROR,
            "Realtek RTL810x network interface registration failed");
        return false;
    }

    if (!rtl810x_init())
        serial_log(
            SERIAL_LOG_WARN,
            "No supported Realtek RTL8102E/RTL8103E controller initialized");
    return true;
}

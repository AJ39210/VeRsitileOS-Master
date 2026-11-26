#include <stdbool.h>
#include <stdint.h>

#include "e1000.h"
#include "module.h"
#include "serial.h"

const uint32_t vos_module_abi_version = 1;

bool vos_module_init(void)
{
    const module_ethernet_ops_t ops = {
        .is_initialized = e1000_is_initialized,
        .link_is_up = e1000_link_is_up,
        .mac_address = e1000_mac_address,
        .send = e1000_send,
        .receive = e1000_receive
    };
    if (!kernel_module_register_ethernet(&ops))
    {
        serial_log(SERIAL_LOG_ERROR, "e1000 network interface registration failed");
        return false;
    }

    if (!e1000_init())
        serial_log(SERIAL_LOG_WARN, "No supported e1000 network controller initialized");
    return true;
}

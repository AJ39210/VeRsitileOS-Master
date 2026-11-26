#include <stdbool.h>
#include <stdint.h>

#include "module.h"
#include "serial.h"
#include "virtio_net.h"

const uint32_t vos_module_abi_version = 1;

bool vos_module_init(void)
{
    const module_ethernet_ops_t ops = {
        .is_initialized = virtio_net_is_initialized,
        .link_is_up = virtio_net_link_is_up,
        .mac_address = virtio_net_mac_address,
        .send = virtio_net_send,
        .receive = virtio_net_receive
    };
    if (!kernel_module_register_virtio_net(&ops))
    {
        serial_log(SERIAL_LOG_ERROR, "VirtIO network interface registration failed");
        return false;
    }

    if (!virtio_net_init())
        serial_log(SERIAL_LOG_INFO, "No supported VirtIO network controller initialized");
    return true;
}

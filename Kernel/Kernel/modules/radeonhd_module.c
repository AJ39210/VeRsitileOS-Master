#include <stdbool.h>
#include <stdint.h>

#include "module.h"
#include "pci.h"
#include "radeonhd.h"
#include "serial.h"

const uint32_t vos_module_abi_version = 1;

bool vos_module_init(void)
{
    if (!pci_is_initialized() || !radeonhd_init())
    {
        serial_log(SERIAL_LOG_ERROR, "Radeon HD probe module initialization failed");
        return false;
    }

    const module_radeonhd_ops_t ops = {
        .format_attribute = radeonhd_format_attribute,
        .format_debug = radeonhd_format_debug
    };
    if (!kernel_module_register_radeonhd(&ops))
    {
        serial_log(SERIAL_LOG_ERROR, "Radeon HD metadata registration failed");
        return false;
    }
    return true;
}

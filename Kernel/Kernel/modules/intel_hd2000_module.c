#include <stdbool.h>
#include <stdint.h>

#include "intel_hd2000.h"
#include "pci.h"
#include "serial.h"

const uint32_t vos_module_abi_version = 1;

bool vos_module_init(void)
{
    if (!pci_is_initialized())
    {
        serial_log(SERIAL_LOG_ERROR, "Intel HD probe module requires initialized PCI");
        return false;
    }
    return intel_hd2000_probe();
}

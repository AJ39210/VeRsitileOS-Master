#include <stdbool.h>
#include <stdint.h>

#include "module.h"
#include "serial.h"
#include "virtio_gpu.h"

const uint32_t vos_module_abi_version = 1;

static bool module_constructor_ran;

__attribute__((constructor))
static void virtio_gpu_module_constructor(void)
{
    module_constructor_ran = true;
}

bool vos_module_init(void)
{
    if (!module_constructor_ran)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "VirtIO GPU module constructor was not executed");
        return false;
    }

    const module_virtio_gpu_ops_t ops = {
        .initialize = virtio_gpu_driver_init,
        .is_initialized = virtio_gpu_driver_is_initialized,
        .present = virtio_gpu_driver_present
    };
    if (!kernel_module_register_virtio_gpu(&ops))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "VirtIO GPU operation registration failed");
        return false;
    }

    if (!virtio_gpu_driver_init())
        serial_log(SERIAL_LOG_INFO,
                   "No supported VirtIO GPU controller initialized");
    return true;
}

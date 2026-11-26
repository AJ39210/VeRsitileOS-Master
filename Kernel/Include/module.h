#ifndef MODULE_H
#define MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radeonhd.h"

typedef struct
{
    size_t (*format_attribute)(
        const pci_device_t *pci_device,
        const char *attribute,
        char *buffer,
        size_t capacity);
    size_t (*format_debug)(
        const pci_device_t *pci_device,
        char *buffer,
        size_t capacity);
} module_radeonhd_ops_t;

typedef struct
{
    bool (*is_initialized)(void);
    bool (*link_is_up)(void);
    const uint8_t *(*mac_address)(void);
    bool (*send)(const void *packet, size_t length);
    size_t (*receive)(void *packet, size_t capacity);
} module_ethernet_ops_t;

typedef struct
{
    bool (*initialize)(void);
    bool (*is_initialized)(void);
    bool (*present)(void);
} module_virtio_gpu_ops_t;

int64_t kernel_module_load(const char *path);
bool kernel_module_register_radeonhd(const module_radeonhd_ops_t *ops);
bool kernel_module_register_ethernet(const module_ethernet_ops_t *ops);
bool kernel_module_register_realtek(const module_ethernet_ops_t *ops);
bool kernel_module_register_virtio_net(const module_ethernet_ops_t *ops);
bool kernel_module_register_virtio_gpu(const module_virtio_gpu_ops_t *ops);

#endif

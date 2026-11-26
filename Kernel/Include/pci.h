#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_DEVICE_LIMIT 256
#define PCI_BDF_NAME_LENGTH 13
#define PCI_BAR_LIMIT 6

typedef struct
{
    uint64_t base;
    uint64_t size;
    bool is_io;
    bool is_64bit;
    bool prefetchable;
} pci_bar_info_t;

typedef struct
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision;
    uint8_t programming_interface;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t bar_count;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t bars[PCI_BAR_LIMIT];
} pci_device_t;

bool pci_init(void);
bool pci_is_initialized(void);
uint32_t pci_device_count(void);
const pci_device_t *pci_device_get(uint32_t index);
bool pci_enable_memory(const pci_device_t *device);
bool pci_enable_device(const pci_device_t *device, bool memory_space,
                       bool bus_master);
bool pci_get_bar_info(const pci_device_t *device, uint8_t bar_index,
                      pci_bar_info_t *info);
uint32_t pci_class_device_count(uint8_t class_code, uint8_t subclass);
const pci_device_t *pci_class_device_get(
    uint8_t class_code,
    uint8_t subclass,
    uint32_t index
);
const pci_device_t *pci_find_bdf(const char *bdf);
const char *pci_vendor_name(uint16_t vendor_id);
const char *pci_device_name(const pci_device_t *device);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);
size_t pci_format_device(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
);
size_t pci_format_attribute(
    const pci_device_t *device,
    const char *attribute,
    char *buffer,
    size_t capacity
);
size_t pci_format_resources(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
);
bool pci_format_bdf(
    const pci_device_t *device,
    char *buffer,
    size_t capacity
);

#endif
